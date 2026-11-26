// A sliver of IOKit: the registry of devices and drivers.
//
// Programs reach IOKit through the "master" port and get further ports for
// the registry entries, iterators and connections they ask for. Most device
// searches come back empty. The power-management root exists because
// long-running programs (every system daemon, any app with a run loop)
// register with it to hear about sleep and wake, and some treat failure as
// fatal. Nothing is ever announced: the guest never sleeps.

#include <algorithm>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/mount.h>

#include <set>
#include <ctime>
#include <unistd.h>
#include <signal.h>
#include <thread>

#include "kernel/input_notify.h"
#include "kernel/mach_message.h"
#include "kernel/process.h"
#include "viewer/viewer.h"

namespace {

enum : u32 {
  kNoDevice = 0xE00002C0,     // "no such device", and how an iterator says it is done
  kUnsupported = 0xE00002C7,
  kNotFound = 0xE00002F0,
};

// What a registry port stands for (KPort::io).
enum : u32 {
  kPowerRoot = 1,
  kConnection,     // to the power root
  kNotification,
  kIterator,       // hands out one object of kind KPort::item, or nothing
  kRegistryRoot,
  kHIDSystem,      // the input system: keyboards and mice, as one event queue
  kHIDConnection,
  kMountedMedia,
  kOpticalMedia,
};

// The input system's shared memory, laid out as Mac OS X 10.5 has it: two
// offsets, then the "event globals", which end in a ring of 240 events. The
// window server maps it and reads events out of the ring; nothing else
// tells it about the mouse or the keyboard.
constexpr u32 kEvGlobals = 8;             // where the globals start
// The globals come in two arrangements. PowerPC programs keep the one 10.4
// had, with the queue's three marks right at the front; Intel ones have the
// marks at the back, just before the queue. Everything between is the same
// list of fields either way, so on PowerPC it all sits twelve bytes later.
struct EvLayout {
  u32 head, tail, last;     // the slot to read next, to write next, and the last written
  u32 event_number, buttons, flags, cursor;
  u32 version, struct_size;
  // Within one event. Both kinds begin with its type, place, time, modifier
  // flags and window, 28 bytes; then PowerPC has the 48 bytes of details and
  // after them the device and process it came from, and Intel has those two
  // first and the details last.
  u32 details;
};
constexpr EvLayout kEvPowerPC = {4, 8, 12, 16, 20, 24, 32, 56, 60, 28};
constexpr EvLayout kEvIntel = {212, 216, 220, 4, 8, 12, 20, 44, 48, 40};

// The kinds of input event, as Mac OS X numbers them.
enum : u32 {
  kLeftDown = 1, kLeftUp = 2, kRightDown = 3, kRightUp = 4,
  kMoved = 5, kLeftDragged = 6, kRightDragged = 7,
  kKeyDown = 10, kKeyUp = 11, kModifiersChanged = 12, kScrollWheel = 22,
};
constexpr u32 kEvQueue = kEvGlobals + 224, kEvQueueLength = 240;  // each slot is 96 bytes

bool mounted_optical_image(const std::string& bsd) {
  std::vector<struct statfs> mounts;
  const int error = mounted_volumes(mounts, MNT_NOWAIT);
  if (error) {
    log_warn("cannot list mounted volumes: %s", strerror(error));
    return false;
  }
  bool mounted = false;
  for (const struct statfs& fs : mounts) {
    if (std::string(fs.f_mntfromname) == "/dev/" + bsd) {
      mounted = true;
      break;
    }
  }
  if (!mounted) return false;
  io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOBSDNameMatching(kIOMainPortDefault, 0, bsd.c_str()));
  if (!media) {
    log_warn("cannot find IOKit media for mounted volume %s", bsd.c_str());
    return false;
  }
  bool optical = IOObjectConformsTo(media, "IOCDMedia") || IOObjectConformsTo(media, "IODVDMedia");
  io_iterator_t parents = IO_OBJECT_NULL;
  const kern_return_t result = IORegistryEntryCreateIterator(
      media, kIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents, &parents);
  if (result != KERN_SUCCESS) {
    log_warn("cannot inspect mounted media %s: 0x%x", bsd.c_str(), result);
  } else {
    io_object_t entry;
    while ((entry = IOIteratorNext(parents))) {
      optical |= IOObjectConformsTo(entry, "AppleDiskImageDevice") ||
                 IOObjectConformsTo(entry, "IOCDMedia") || IOObjectConformsTo(entry, "IODVDMedia");
      IOObjectRelease(entry);
    }
    IOObjectRelease(parents);
  }
  IOObjectRelease(media);
  return optical;
}

std::string matching_bsd_name(const std::string& xml) {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8*)xml.data(), (CFIndex)xml.size());
  if (!data) return "";
  CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
                                                          kCFPropertyListImmutable, nullptr, nullptr);
  CFRelease(data);
  std::string result;
  if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID() &&
      CFDictionaryGetCount((CFDictionaryRef)plist) == 1) {
    CFTypeRef properties = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("IOPropertyMatch"));
    if (properties && CFGetTypeID(properties) == CFDictionaryGetTypeID() &&
        CFDictionaryGetCount((CFDictionaryRef)properties) == 1) {
      CFTypeRef name = CFDictionaryGetValue((CFDictionaryRef)properties, CFSTR("BSD Name"));
      char bsd[128];
      if (name && CFGetTypeID(name) == CFStringGetTypeID() &&
          CFStringGetCString((CFStringRef)name, bsd, sizeof bsd, kCFStringEncodingUTF8))
        result = bsd;
    }
  }
  if (plist) CFRelease(plist);
  return result;
}

}  // namespace

bool Process::iokit_rpc(u32 name, const KPort& port, u32 msg, u32 size, std::vector<u8>& reply) {
  const bool be = mem_.big_endian();
  const u32 local = mem_.r32(msg + 12);
  const s32 id = (s32)mem_.r32(msg + 20);
  auto code = [&](u32 value) {
    Message m(be, local, id + 100);
    m.ndr();
    m.put(value);
    reply = m.finish();
    return true;
  };
  // A new registry object, handed over as a port.
  auto object = [&](u32 kind) {
    u32 made;
    {
      std::lock_guard<std::mutex> lk(kmu_);
      made = new_kport(KObject::IOObject);
      kports_[made].io = kind;
    }
    give_send(made);
    Message m(be, local, id + 100, true);
    m.put(1);
    m.port(made, kMoveSend);
    reply = m.finish();
    return true;
  };
  // A string argument: two counts, then the characters, starting at `at`.
  auto string_at = [&](u32 at) {
    std::string s;
    const u32 n = at + 8 <= size ? mem_.r32(msg + at + 4) : 0;
    for (u32 i = 0; i < n && at + 8 + i < size; i++) {
      const char c = (char)mem_.r8(msg + at + 8 + i);
      if (!c) break;
      s += c;
    }
    return s;
  };

  // An iterator over one object of the given kind (or none, for 0).
  auto iterator = [&](u32 item) {
    u32 made;
    {
      std::lock_guard<std::mutex> lk(kmu_);
      made = new_kport(KObject::IOObject);
      kports_[made].io = kIterator;
      kports_[made].item = item;
      kports_[made].initial_item = item;
    }
    give_send(made);
    Message m(be, local, id + 100, true);
    m.put(1);
    m.port(made, kMoveSend);
    reply = m.finish();
    return true;
  };
  // The answer to a driver method, in whichever of the call's shapes.
  auto arg = [&](u32 n) -> u32 { return 32 + 4 * n + 4 <= size ? mem_.r32(msg + 32 + 4 * n) : 0; };
  // Direct kernel RPCs have not passed through Mach's right transfer. Only
  // a moved right belongs to the receiver to discard; a copied or made
  // right leaves the sender's reference untouched.
  auto discard_port = [&]() -> kern_return_t {
    if (size < 40) return KERN_INVALID_ARGUMENT;
    const u32 given = mem_.r32(msg + 28);
    if (!given) return KERN_SUCCESS;
    switch (mem_.r8(msg + 38)) {
      case MACH_MSG_TYPE_MOVE_SEND:
      case MACH_MSG_TYPE_MOVE_SEND_ONCE:
        return mach_port_deallocate(mach_task_self(), given);
      case MACH_MSG_TYPE_COPY_SEND:
      case MACH_MSG_TYPE_MAKE_SEND:
      case MACH_MSG_TYPE_MAKE_SEND_ONCE:
        return KERN_SUCCESS;
      default:
        return KERN_INVALID_ARGUMENT;
    }
  };

  switch (id) {
    case 2800: {  // io_object_get_class
      const char* cls = port.io == kPowerRoot  ? "IOPMrootDomain"
                        : port.io == kHIDSystem ? "IOHIDSystem"
                        : port.io == kOpticalMedia ? "IOCDMedia"
                        : port.io == kMountedMedia ? "IOMedia"
                                                : "IOService";
      Message m(be, local, id + 100);
      m.ndr();
      m.put(KERN_SUCCESS);
      const u32 n = (u32)strlen(cls) + 1;
      m.put(0);
      m.put(n);
      for (u32 i = 0; i < ((n + 3) & ~3u); i += 4) {
        char word[4] = {};
        for (u32 k = 0; k < 4 && i + k < n; k++) word[k] = cls[i + k];
        u32 v;
        memcpy(&v, word, 4);
        m.put(be ? __builtin_bswap32(v) : v);  // put() swaps back: the bytes go out in order
      }
      reply = m.finish();
      return true;
    }
    case 2801: {  // io_object_conforms_to
      const std::string cls = string_at(32);
      const bool media = port.io == kMountedMedia || port.io == kOpticalMedia;
      if (!media) return false;
      const bool conforms = cls == "IOMedia" || cls == "IOService" ||
                            (port.io == kOpticalMedia && cls == "IOCDMedia");
      Message m(be, local, id + 100);
      m.ndr();
      m.put(KERN_SUCCESS);
      m.put(conforms ? 1 : 0);
      reply = m.finish();
      return true;
    }
    case 2802: {  // io_iterator_next: the one object, once
      u32 item = 0;
      {
        std::lock_guard<std::mutex> lk(kmu_);
        const auto it = kports_.find(name);
        if (it != kports_.end()) {
          item = it->second.item;
          it->second.item = 0;
        }
      }
      return item ? object(item) : code(kNoDevice);
    }
    case 2827:  // io_registry_get_root_entry
      return object(kRegistryRoot);
    case 2831:  // io_service_get_busy_state: nothing is ever still starting up
      return [&] {
        Message m(be, local, id + 100);
        m.ndr();
        m.put(KERN_SUCCESS);
        m.put(0);
        reply = m.finish();
        return true;
      }();
    case 2832:  // io_service_wait_quiet
      return code(KERN_SUCCESS);
    case 2818: {  // io_connect_set_notification_port(type, port, reference)
      const u32 given = size >= 40 ? mem_.r32(msg + 28) : 0;
      if (port.io == kHIDConnection) {
        // The guest hands over a right to send to a port it receives on. Its
        // request never travelled anywhere, so the host kernel hasn't made
        // that right: it is made here, or nothing could be sent to the port.
        const u32 how = size >= 40 ? mem_.r8(msg + 38) : 0;
        if (given && (how == MACH_MSG_TYPE_MAKE_SEND || how == MACH_MSG_TYPE_MAKE_SEND_ONCE)) {
          mach_port_insert_right(mach_task_self(), given, given, MACH_MSG_TYPE_MAKE_SEND);
        }
        hid_notify_ = given;  // told when events are waiting
      } else {
        const kern_return_t kr = discard_port();
        if (kr != KERN_SUCCESS) return code(kr);
      }
      return code(KERN_SUCCESS);
    }
    case 2819: {  // io_connect_map_memory(type, into task, address, size, flags) -> address, size
      const kern_return_t kr = discard_port();
      if (kr != KERN_SUCCESS) return code(kr);
      u32 address = 0, bytes = 0;
      if (!io_memory(port, size >= 52 ? mem_.r32(msg + 48) : 0, address, bytes)) return code(kUnsupported);
      Message m(be, local, id + 100);
      m.ndr();
      m.put(KERN_SUCCESS);
      m.put(address);
      m.put(bytes);
      reply = m.finish();
      return true;
    }
    case 2822:
    case 2823:
    case 2824:
    case 2825:
    case 2865: {  // the ways of calling a driver method
      // Each carries a selector, some numbers and some bytes in, and wants
      // numbers and bytes back; they differ in which of those exist and how
      // wide the numbers are.
      std::vector<u64> in, out;
      std::vector<u8> bytes_in, bytes_out;
      const u32 selector = arg(0);
      u32 at = 36;  // past the header, the record and the selector
      auto word = [&]() -> u32 {
        const u32 v = at + 4 <= size ? mem_.r32(msg + at) : 0;
        at += 4;
        return v;
      };
      auto take_bytes = [&](u32 n) {
        for (u32 i = 0; i < n && at + i < size; i++) bytes_in.push_back(mem_.r8(msg + at + i));
        at += (n + 3) & ~3u;
      };
      u32 want_numbers = 0, want_bytes = 0;
      if (id == 2865) {
        const u32 n = word();
        for (u32 i = 0; i < n && i < 16; i++) {
          const u64 a = word(), b = word();
          in.push_back(be ? (a << 32) | b : (b << 32) | a);
        }
        take_bytes(word());
        at += 16;  // out-of-line input: address and size, 64 bits each; not used here
        want_bytes = word();
        want_numbers = word();
      } else {
        if (id == 2822 || id == 2823 || id == 2824) {
          const u32 n = word();
          for (u32 i = 0; i < n && i < 16; i++) in.push_back(word());
        }
        if (id == 2824 || id == 2825) take_bytes(word());
        if (id == 2822) want_numbers = word();
        if (id == 2823 || id == 2825) want_bytes = word();
      }
      const u32 kr = io_method(port, selector, in, bytes_in, out, bytes_out);
      if (kr != KERN_SUCCESS) return code(kr);
      out.resize(want_numbers < 16 ? want_numbers : 16);
      bytes_out.resize(want_bytes < 4096 ? want_bytes : 4096);
      const size_t padded_bytes = (bytes_out.size() + 3) & ~size_t(3);
      size_t capacity = 40;
      if (id == 2865) {
        capacity = 52 + padded_bytes + 8 * out.size();
      } else if (id == 2822) {
        capacity = 40 + 4 * out.size();
      } else if (id == 2823 || id == 2825) {
        capacity = 40 + padded_bytes;
      }
      Message m(be, local, id + 100, false, capacity);
      m.ndr();
      m.put(KERN_SUCCESS);
      auto put_bytes = [&] {
        m.put((u32)bytes_out.size());
        m.put_bytes(bytes_out);
      };
      if (id == 2865) {
        put_bytes();
        m.put((u32)out.size());
        for (u64 v : out) m.put64(v);
        m.put64(0);  // out-of-line output size
      } else if (id == 2822) {
        m.put((u32)out.size());
        for (u64 v : out) m.put((u32)v);
      } else if (id == 2823 || id == 2825) {
        put_bytes();
      }
      reply = m.finish();
      return true;
    }
    case 2803: {  // io_iterator_reset
      std::lock_guard<std::mutex> lk(kmu_);
      const auto it = kports_.find(name);
      if (it != kports_.end()) it->second.item = it->second.initial_item;
      return code(KERN_SUCCESS);
    }
    case 2816:  // io_service_close
      return code(KERN_SUCCESS);
    case 2804: {  // io_service_get_matching_services(what to match) -> iterator
      // Other searches are noted (with -v), as the list of devices programs expect.
      const std::string what = string_at(32);
      const std::string bsd = matching_bsd_name(what);
      if (!bsd.empty() && mounted_optical_image(bsd)) return iterator(kMountedMedia);
      static std::set<std::string> seen;
      static std::mutex seen_mu;
      std::lock_guard<std::mutex> lk(seen_mu);
      if (what.find(">IOHIDSystem<") != std::string::npos) return iterator(kHIDSystem);
      if (seen.insert(what).second) log_info("a program searched IOKit for %s", what.c_str());
      return iterator(0);
    }
    case 2833: {  // io_registry_entry_create_iterator
      const u32 n = size >= 40 ? mem_.r32(msg + 36) : 0;
      const u32 options_at = 40 + ((n + 3) & ~3u);
      if (port.io == kMountedMedia && size >= 40 && n <= size - 40 && options_at + 4 <= size &&
          string_at(32) == "IOService" && mem_.r32(msg + options_at) == 3)
        return iterator(kOpticalMedia);
      return code(kUnsupported);
    }
    case 2835: {  // io_make_matching: Leopard's IOBSDNameMatching
      if (size < 45 || mem_.r32(msg + 32) != 101) return code(kUnsupported);
      const u32 n = mem_.r32(msg + 40);
      if (!n || n > size - 44 || n > 127) return code(kUnsupported);
      std::string bsd;
      for (u32 i = 0; i < n && mem_.r8(msg + 44 + i); i++) {
        const char c = (char)mem_.r8(msg + 44 + i);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'))
          return code(kUnsupported);
        bsd += c;
      }
      if (bsd.empty()) return code(kUnsupported);
      const std::string xml = "<dict><key>IOPropertyMatch</key><dict><key>BSD Name</key><string>" +
                              bsd + "</string></dict></dict>";
      Message m(be, local, id + 100);
      m.ndr();
      m.put(KERN_SUCCESS);
      m.put(0);
      m.put((u32)xml.size() + 1);
      for (size_t i = 0; i <= xml.size(); i += 4) {
        u8 bytes[4] = {};
        for (size_t j = 0; j < 4 && i + j < xml.size(); j++) bytes[j] = (u8)xml[i + j];
        u32 word;
        memcpy(&word, bytes, 4);
        m.put(be ? __builtin_bswap32(word) : word);
      }
      reply = m.finish();
      return true;
    }
    case 2809: {  // io_registry_entry_from_path(path) -> entry
      const std::string path = string_at(32);
      if (path.find("IOPMrootDomain") != std::string::npos) return object(kPowerRoot);
      if (path == "IOService:/") return object(kRegistryRoot);
      log_info("a program asked IOKit for the registry entry %s", path.c_str());
      return code(kNotFound);
    }
    case 2815: {  // io_service_open(owning task, type) -> connection
      const kern_return_t kr = discard_port();
      if (kr != KERN_SUCCESS) return code(kr);
      if (port.io != kPowerRoot && port.io != kHIDSystem) return code(kUnsupported);
      return object(port.io == kHIDSystem ? kHIDConnection : kConnection);
    }
    case 2862: {  // io_service_open_extended(owning task, properties, type) -> result, connection
      const kern_return_t kr = discard_port();
      if (kr != KERN_SUCCESS) return code(kr);
      if (port.io != kPowerRoot && port.io != kHIDSystem) return code(kUnsupported);
      u32 made;
      {
        std::lock_guard<std::mutex> lk(kmu_);
        made = new_kport(KObject::IOObject);
        kports_[made].io = port.io == kHIDSystem ? kHIDConnection : kConnection;
      }
      give_send(made);
      Message m(be, local, id + 100, true);
      m.put(1);
      m.port(made, kMoveSend);
      m.ndr();
      m.put(KERN_SUCCESS);  // the driver's own verdict, separate from the call's
      reply = m.finish();
      return true;
    }
    case 2849:  // io_service_add_notification
    case 2850:  // io_service_add_interest_notification(what, port to tell) -> notification
      // The port stays held, and silent: there is never anything to tell.
      return object(kNotification);
    case 2851:  // io_service_acknowledge_notification
      return code(KERN_SUCCESS);
    case 2805: {  // io_registry_entry_get_property(name) -> value
      if (port.io == kOpticalMedia && string_at(32) == "Whole") {
        const char value[] = "<true/>";
        const u32 at = mem_.find_free(0x1000, 0x1000, 0xC0000000u);
        if (!at || !mem_.map(at, 0x1000, GuestMemory::kRead | GuestMemory::kWrite))
          return code(KERN_NO_SPACE);
        memcpy(mem_.host(at), value, sizeof value);
        Message m(be, local, id + 100, true);
        m.put(1);
        m.ool_data(at, sizeof value);
        m.ndr();
        m.put(sizeof value);
        reply = m.finish();
        return true;
      }
      // The menu bar also asks the root for IOConsoleUsers, the login sessions.
      return code(kNotFound);
    }
  }
  (void)name;
  return false;
}

u32 Process::io_method(const KPort& port, u32 selector, const std::vector<u64>& in,
                       const std::vector<u8>& bytes_in, std::vector<u64>& out,
                       std::vector<u8>& bytes_out) {
  (void)bytes_in, (void)out, (void)bytes_out;
  if (port.io == kHIDConnection) {
    switch (selector) {
      case 0: {  // create the shared memory: the argument is the layout version the client knows
        log_info("input system: the client asks for event-queue layout version %llu",
                 in.empty() ? 0ull : (unsigned long long)in[0]);
        if (hid_memory_) return KERN_SUCCESS;
        const u32 globals_size = 224 + hid_element_ * kEvQueueLength;
        const u32 bytes = (kEvGlobals + globals_size + 0xFFF) & ~0xFFFu;
        const u32 at = mem_.find_free(bytes, 0x1000, 0xC0000000u);
        if (!at || !mem_.map(at, bytes, GuestMemory::kRead | GuestMemory::kWrite)) return KERN_NO_SPACE;
        mem_.w32(at, kEvGlobals);                   // where the globals are
        mem_.w32(at + 4, kEvGlobals + globals_size);  // and the private area after them
        const EvLayout& l = mem_.big_endian() ? kEvPowerPC : kEvIntel;
        const u32 g = at + kEvGlobals;
        mem_.w32(g + l.version, in.empty() ? 0 : (u32)in[0]);
        mem_.w32(g + l.struct_size, globals_size);
        // The queue is a ring: each slot names the next. Empty when the
        // place to read is the place to write.
        for (u32 i = 0; i < kEvQueueLength; i++) {
          mem_.w32(at + kEvQueue + hid_element_ * i, (i + 1) % kEvQueueLength);
        }
        mem_.w32(g + l.last, 0);
        mem_.w32(g + l.head, 1);
        mem_.w32(g + l.tail, 1);
        hid_memory_ = at;
        hid_memory_size_ = bytes;
        // For working on cocaine, and its tests: a script of mouse and key
        // actions to play into the queue (see run_input_script).
        std::string script;
        if (const char* e = getenv("COCAINE_SCRIPT")) script = e;
        if (const char* poke = getenv("COCAINE_POKE")) {
          // The short form, x,y,seconds: after that long click there, then type 8 and 9.
          int x = 0, y = 0, after = 0;
          if (sscanf(poke, "%d,%d,%d", &x, &y, &after) == 3) {
            char text[160];
            snprintf(text, sizeof text, "wait %d; move %d %d; wait 0.2; click %d %d; wait 1; key 28 56; wait 0.3; key 25 57",
                     after, x - 20, y - 20, x, y);
            script = text;
          }
        }
        if (!script.empty()) {
          sigset_t everything, before;
          sigfillset(&everything);
          pthread_sigmask(SIG_SETMASK, &everything, &before);
          std::thread([this, script] { run_input_script(script); }).detach();
          pthread_sigmask(SIG_SETMASK, &before, nullptr);
        }
        return KERN_SUCCESS;
      }
      case 4: {  // the window server moving the pointer itself (IOHIDSetMouseLocation)
        // Where to, as two 16-bit numbers in the guest's byte order, then
        // the process asking.
        if (bytes_in.size() >= 4) {
          const bool be = mem_.big_endian();
          auto half = [&](size_t at) {
            return (s16)(be ? bytes_in[at] << 8 | bytes_in[at + 1] : bytes_in[at + 1] << 8 | bytes_in[at]);
          };
          set_pointer(half(0), half(2));
        }
        return KERN_SUCCESS;
      }
      default:  // enabling events and the cursor, setting bounds and the like
        log_info("input system method %u (%zu numbers, %zu bytes in) accepted", selector, in.size(),
                 bytes_in.size());
        return KERN_SUCCESS;
    }
  }
  log_info("a program called method %u of an IOKit driver that has none", selector);
  return kUnsupported;
}

bool Process::io_memory(const KPort& port, u32 type, u32& address, u32& size) {
  if (port.io == kHIDConnection && type == 0 && hid_memory_) {
    address = hid_memory_;
    size = hid_memory_size_;
    return true;
  }
  return false;
}

// Plays a script of input into the queue, for working on cocaine and for its
// tests. Commands are separated by semicolons:
//
//   wait SECONDS              (may be a fraction)
//   move X Y                  the pointer goes there
//   down X Y, up X Y          the left button
//   dragto X Y                the pointer goes there with the button held
//   click X Y [N]             down and up; N is which click of a run (2: a double click's second)
//   drag X1 Y1 X2 Y2 STEPS MS press at the first point, move to the second in
//                             that many steps MS milliseconds apart, let go
//   key CODE CHARACTER [MODIFIERS]   a key down and up; numbers, modifiers in hexadecimal
//   settle                    wait for the window server to have read everything,
//                             and log how long that took and how far behind it got
//   change X Y W H            wait for that part of the display to change, and
//                             log how long it took (measures from the command before)
//   shot FILE                 write the published display to FILE (four bytes each,
//                             a display's width to a row), if this process holds it
void Process::run_input_script(const std::string& script) {
  auto pause = [](double seconds) { usleep((useconds_t)(seconds * 1e6)); };
  auto depth = [this]() -> u32 {
    const EvLayout& l = mem_.big_endian() ? kEvPowerPC : kEvIntel;
    const u32 g = hid_memory_ + kEvGlobals;
    return (mem_.r32(g + l.tail) + kEvQueueLength - mem_.r32(g + l.head)) % kEvQueueLength;
  };
  u32 worst = 0;
  auto post = [&](u32 type, s32 x, s32 y, u32 key = 0, u32 extra = 0, s32 more = 0) {
    post_input_event(type, x, y, key, extra, more);
    worst = std::max(worst, depth());
  };
  size_t at = 0;
  while (at < script.size()) {
    size_t end = script.find(';', at);
    if (end == std::string::npos) end = script.size();
    const std::string line = script.substr(at, end - at);
    at = end + 1;
    char word[32] = "", text[512] = "";
    double v[6] = {};
    const int n = sscanf(line.c_str(), " %31s %lf %lf %lf %lf %lf %lf", word, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n < 1) continue;
    const std::string w = word;
    const s32 x = (s32)v[0], y = (s32)v[1];
    if (w == "wait") {
      pause(v[0]);
    } else if (w == "move") {
      post(kMoved, x, y);
    } else if (w == "dragto") {
      post(kLeftDragged, x, y);
    } else if (w == "down") {
      post(kLeftDown, x, y, 0, 0, 1);
    } else if (w == "up") {
      post(kLeftUp, x, y, 0, 0, 1);
    } else if (w == "click") {
      const s32 count = n >= 4 ? (s32)v[2] : 1;
      post(kMoved, x, y);
      pause(0.05);
      post(kLeftDown, x, y, 0, 0, count);
      pause(0.1);
      post(kLeftUp, x, y, 0, 0, count);
    } else if (w == "drag") {
      const int steps = std::max(1, (int)v[4]);
      post(kMoved, x, y);
      pause(0.05);
      post(kLeftDown, x, y, 0, 0, 1);
      pause(0.1);
      for (int i = 1; i <= steps; i++) {
        post(kLeftDragged, (s32)(v[0] + (v[2] - v[0]) * i / steps), (s32)(v[1] + (v[3] - v[1]) * i / steps));
        pause(v[5] / 1000);
      }
      post(kLeftUp, (s32)v[2], (s32)v[3], 0, 0, 1);
    } else if (w == "key") {
      unsigned modifiers = 0;
      sscanf(line.c_str(), " %*s %*s %*s %x", &modifiers);
      const u32 extra = (modifiers & 0xFFFF0000u) | ((u32)v[1] & 0xFFFF);
      post(kKeyDown, hid_x_, hid_y_, (u32)v[0], extra);
      pause(0.1);
      post(kKeyUp, hid_x_, hid_y_, (u32)v[0], extra);
    } else if (w == "settle") {
      const u64 from = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      while (depth() != 0 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - from < 30000000000ull) usleep(2000);
      log_warn("input script: the window server caught up after %llu ms; it had been up to %u events behind",
               (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - from) / 1000000), worst);
      worst = 0;
    } else if (w == "change") {
      // change X Y WIDTH HEIGHT: wait (up to ten seconds) for that part of
      // the display to look different, and log how long it took.
      const u32 rx = (u32)v[0], ry = (u32)v[1], rw = (u32)v[2], rh = (u32)v[3];
      u32 dw = 0, dh = 0;
      if (display_file_) {
        std::lock_guard<std::mutex> lk(publish_mu_);
        dw = display_w_;
        dh = display_h_;
      }
      if (!display_file_ || (u64)rx + rw > dw || (u64)ry + rh > dh || !rw || !rh) continue;
      auto grab = [&] {
        std::vector<u8> px;
        std::lock_guard<std::mutex> lk(publish_mu_);
        if ((u64)rx + rw > display_w_ || (u64)ry + rh > display_h_) return px;
        const u8* pixels = display_file_ + sizeof(DisplayFileHeader);
        for (u32 row = 0; row < rh; row++) {
          const u8* from = pixels + ((size_t)(ry + row) * display_w_ + rx) * 4;
          px.insert(px.end(), from, from + rw * 4);
        }
        return px;
      };
      const std::vector<u8> before = grab();
      const u64 from = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      bool changed = false;
      while (!changed && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - from < 10000000000ull) {
        usleep(1000);
        changed = grab() != before;
      }
      log_warn("input script: the display at %u,%u %s after %llu ms", rx, ry,
               changed ? "changed" : "had not changed",
               (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - from) / 1000000));
    } else if (w == "shot" && sscanf(line.c_str(), " %*s %511s", text) == 1) {
      u32 dw = 0, dh = 0;
      if (display_file_) {
        std::lock_guard<std::mutex> lk(publish_mu_);
        dw = display_w_;
        dh = display_h_;
        const u32 bytes = dw * dh * 4;
        if (FILE* f = fopen(text, "wb")) {
          if (fwrite(display_file_ + sizeof(DisplayFileHeader), 1, bytes, f) != bytes)
            log_warn("input script: could not write the display picture to %s", text);
          fclose(f);
        } else log_warn("input script: could not open %s for the display picture", text);
      } else {
        log_warn("input script: no display to take a picture of yet");
      }
    } else {
      log_warn("input script: unknown command '%s'", word);
    }
  }
}

// What the kernel's input system does when a mouse or key does something:
// fills in the next free slot of the ring, moves the ring's marks along, and
// sends the window server an empty message so that it looks.
//
// A game that looks around with the mouse hides the pointer and, after each
// movement, puts it back in the middle of its window, reading how far the
// mouse moved from the events. The Mac's pointer goes on wherever the user
// moves it, so from the next movement on, the pointer would jump from the
// middle straight back to it, which the game takes for one huge movement.
// So, as the real kernel does, the pointer is kept where the window server
// put it, and while it keeps doing so (or did, lately), each movement moves
// the pointer that far from there instead of to where the Mac's pointer is.
// The viewer, seeing the pointer held, hides the Mac's own and keeps it still.
void Process::set_pointer(s32 x, s32 y) {
  std::lock_guard<std::mutex> lk(hid_mu_);
  const u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  hid_x_ = x;
  hid_y_ = y;
  hid_relative_until_ = now + 1500000000ull;
  // Twice in half a second is a program holding the pointer, not putting it
  // somewhere once (as the window server does when it starts).
  if (display_file_ && hid_last_warp_ && now - hid_last_warp_ < 500000000ull) {
    auto* head = (DisplayFileHeader*)display_file_;
    __atomic_store_n(&head->pointer_held, std::max<u32>(1, (u32)(now / 1000000)), __ATOMIC_RELEASE);
  }
  hid_last_warp_ = now;
  if (hid_memory_) {
    const EvLayout& l = mem_.big_endian() ? kEvPowerPC : kEvIntel;
    const u32 g = hid_memory_ + kEvGlobals;
    mem_.w16(g + l.cursor, (u16)x);
    mem_.w16(g + l.cursor + 2, (u16)y);
  }
}

// `type` is the event's number as Mac OS X has it (above); x and y are where
// the pointer is on the display. For a movement, `more` is 1 when `key` and
// `extra` say how far the mouse moved, as the Mac measured it (from the
// viewer; a script gives only where to, and the distance is worked out). For a key, `key` is its key code, `extra`
// the modifier keys held (the top sixteen bits, as Mac OS X numbers them)
// with the character below, and `more` is not zero for a key that is
// repeating. For a mouse button, `more` is which click this is: 2 for the
// second of a double click. For the scroll wheel, `key` and `more` are how
// many lines down and across.
void Process::post_input_event(u32 type, s32 x, s32 y, u32 key, u32 extra, s32 more) {
  std::lock_guard<std::mutex> lk(hid_mu_);
  if (type == kViewerCaptureChanged) {
    hid_viewer_captured_ = x != 0;
    return;
  }
  if (!hid_memory_) return;
  if (getenv("COCAINE_NOTE_INPUT") && type != kMoved) {  // for working on cocaine: note what follows
    note_from_ = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    note_sends_until_ = note_from_ + 3000000000ull;
  }
  const EvLayout& l = mem_.big_endian() ? kEvPowerPC : kEvIntel;
  const u32 g = hid_memory_ + kEvGlobals;
  const bool moved = type == kMoved || type == kLeftDragged || type == kRightDragged;
  const bool held = hid_viewer_captured_ || clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < hid_relative_until_;
  s32 dx = 0, dy = 0;
  if (type == kKeyDown || type == kKeyUp || type == kModifiersChanged) {
    // Host keyboard events do not carry a meaningful pointer position.
    x = hid_x_;
    y = hid_y_;
  } else if (moved) {
    dx = more == 1 ? (s32)key : x - hid_x_;
    dy = more == 1 ? (s32)extra : y - hid_y_;
    if (held) {  // from where the window server put the pointer (see set_pointer)
      u32 w = 0, h = 0;
      if (display_file_) {
        const auto* display = (const DisplayFileHeader*)display_file_;
        w = __atomic_load_n(&display->width, __ATOMIC_RELAXED);
        h = __atomic_load_n(&display->height, __ATOMIC_RELAXED);
      } else {
        guest_display_size(w, h);
      }
      x = std::clamp(hid_x_ + dx, 0, (s32)w - 1);
      y = std::clamp(hid_y_ + dy, 0, (s32)h - 1);
    }
  } else if (held) {
    x = hid_x_;  // a button or the wheel, where the guest's pointer is
    y = hid_y_;
  }
  const u32 head = mem_.r32(g + l.head), tail = mem_.r32(g + l.tail);
  if (moved && head != tail) {
    // The pointer moved again before the window server read about the last
    // move. As the real kernel does, the event still waiting is brought up
    // to date rather than another queued behind it: a reader that falls
    // behind then catches up in one step instead of replaying every position
    // the pointer passed through. Each slot has a lock, which the reader
    // holds while it copies an event out; a slot being read is left alone.
    const u32 waiting = hid_memory_ + kEvQueue + hid_element_ * mem_.r32(g + l.last);
    auto* lock = (u32*)mem_.host(waiting + 4);
    u32 unlocked = 0;
    if (__atomic_compare_exchange_n(lock, &unlocked, 0x01010101u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      const bool same = mem_.r32(waiting + 8) == type;
      if (same) {
        const u32 at = waiting + 8 + l.details;
        mem_.w32(at, mem_.r32(at) + (u32)dx);
        mem_.w32(at + 4, mem_.r32(at + 4) + (u32)dy);
        mem_.w32(waiting + 8 + 4, (u32)x);
        mem_.w32(waiting + 8 + 8, (u32)y);
        mem_.w64(waiting + 8 + 12, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        mem_.w16(g + l.cursor, (u16)x);
        mem_.w16(g + l.cursor + 2, (u16)y);
        hid_x_ = x;
        hid_y_ = y;
      }
      __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
      if (same) return;
    }
  }
  const u32 slot = hid_memory_ + kEvQueue + hid_element_ * tail;
  const u32 next = mem_.r32(slot);
  if (next == head) return;  // full: the window server isn't reading
  const u32 ev = slot + 8;
  const u32 details = ev + l.details;
  memset(mem_.host(ev), 0, hid_element_ - 8);
  mem_.w32(ev, type);
  mem_.w32(ev + 4, (u32)x);
  mem_.w32(ev + 8, (u32)y);
  mem_.w64(ev + 12, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
  switch (type) {
    case kKeyDown:
    case kKeyUp: {
      const u32 ch = extra & 0xFFFF;
      mem_.w32(g + l.flags, extra & 0xFFFF0000u);  // the modifiers now held
      mem_.w16(details + 2, more ? 1 : 0);         // a repeat
      mem_.w16(details + 6, (u16)ch);              // the character, in the plain ASCII set
      mem_.w16(details + 8, (u16)key);
      mem_.w16(details + 10, (u16)ch);
      mem_.w32(details + 16, 40);                  // a plain USB keyboard
      break;
    }
    case kModifiersChanged:
      mem_.w32(g + l.flags, extra & 0xFFFF0000u);
      mem_.w16(details + 8, (u16)key);
      break;
    case kLeftDown:
    case kLeftUp:
    case kRightDown:
    case kRightUp: {  // a button: which press this is, which click of a run, and how hard
      const bool down = type == kLeftDown || type == kRightDown;
      const u32 bit = type == kLeftDown || type == kLeftUp ? 1 : 2;
      if (down) hid_event_number_++;
      hid_buttons_ = down ? hid_buttons_ | bit : hid_buttons_ & ~bit;
      mem_.w16(details + 2, (u16)hid_event_number_);
      mem_.w32(details + 4, more > 0 ? (u32)more : 1);
      mem_.w8(details + 8, down ? 255 : 0);
      mem_.w32(g + l.buttons, hid_buttons_);
      mem_.w32(g + l.event_number, hid_event_number_);
      break;
    }
    case kMoved:
    case kLeftDragged:
    case kRightDragged:  // how far it moved
      mem_.w32(details, (u32)dx);
      mem_.w32(details + 4, (u32)dy);
      break;
    case kScrollWheel:  // lines; the window server works the finer measures out from these
      mem_.w16(details, (u16)(s16)(s32)key);
      mem_.w16(details + 2, (u16)(s16)more);
      break;
  }
  mem_.w32(ev + 20, mem_.r32(g + l.flags));
  hid_x_ = x;
  hid_y_ = y;
  mem_.w16(g + l.cursor, (u16)x);  // where the cursor is
  mem_.w16(g + l.cursor + 2, (u16)y);
  mem_.w32(g + l.last, tail);   // the last event entered
  // The window server reads the ring on another thread: the event has to be
  // all there before the mark that says it is.
  __atomic_thread_fence(__ATOMIC_RELEASE);
  mem_.w32(g + l.tail, next);   // where the next will go
  if (hid_notify_) {
    // A full notification port already has a wake-up waiting; don't block
    // the input reader behind it while more events arrive.
    const kern_return_t kr = notify_input(hid_notify_);
    if (kr != KERN_SUCCESS && kr != MACH_SEND_TIMED_OUT) {
      log_warn("could not tell the window server about input: %s", mach_error_string(kr));
    }
  }
}
