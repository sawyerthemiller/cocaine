// Guest Mach messages, carried over real host Mach ports (see tunnel.h).

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <ctime>

#include "kernel/mach_message.h"
#include "kernel/process.h"
#include "kernel/tunnel.h"
#include "session/pacing.h"

extern char** environ;

namespace {

inline u32 rd(const u8* p, bool be) {
  u32 v;
  memcpy(&v, p, 4);
  return be ? __builtin_bswap32(v) : v;
}
inline void wr(u8* p, u32 v, bool be) {
  if (be) v = __builtin_bswap32(v);
  memcpy(p, &v, 4);
}

template <typename T>
void append(std::vector<u8>& out, const T& v) {
  const u8* p = (const u8*)&v;
  out.insert(out.end(), p, p + sizeof v);
}

// The kinds of descriptor in a guest message, by the last byte of each.
enum : u8 { kPortDescriptor = 0, kOolDescriptor = 1, kOolPortsDescriptor = 2, kOolVolatile = 3 };
constexpr u32 kGuestHeader = 24, kGuestDescriptor = 12;

}  // namespace

u32 Process::ipc_send(const u8* g, u32 size, u32 option, u32 timeout) {
  const bool be = mem_.big_endian();
  const u32 bits = rd(g, be);

  std::vector<u8> out;
  out.reserve(sizeof(mach_msg_header_t) + 64 + size);
  mach_msg_header_t h = {};
  h.msgh_bits = MACH_MSGH_BITS(bits & 0x1F, (bits >> 8) & 0x1F) | MACH_MSGH_BITS_COMPLEX;
  h.msgh_remote_port = rd(g + 8, be);
  h.msgh_local_port = rd(g + 12, be);
  h.msgh_id = (mach_msg_id_t)rd(g + 20, be);
  append(out, h);
  mach_msg_body_t body = {0};

  // Arrays of port names have to be rewritten in the host's layout, and
  // memory the sender gives away is unmapped once the message has gone.
  std::vector<std::vector<mach_port_t>> name_arrays;
  struct Range {
    u32 addr, size;
  };
  std::vector<Range> given_away;
  // Where each out-of-line descriptor sits in `out`, and the address it had.
  struct Carried {
    size_t at;
    const void* address;
    bool ports;
  };
  std::vector<Carried> carried;

  if (bits & kComplex) {
    if (size < kGuestHeader + 4) return MACH_SEND_MSG_TOO_SMALL;
    const u32 count = rd(g + kGuestHeader, be);
    if ((u64)kGuestHeader + 4 + (u64)kGuestDescriptor * count > size) return MACH_SEND_MSG_TOO_SMALL;
    body.msgh_descriptor_count = count;
    append(out, body);
    for (u32 i = 0; i < count; i++) {
      const u8* d = g + kGuestHeader + 4 + kGuestDescriptor * i;
      const u32 first = rd(d, be), second = rd(d + 4, be);
      switch (d[11]) {
        case kPortDescriptor: {
          mach_msg_port_descriptor_t p = {};
          p.name = first;
          p.disposition = d[10];
          p.type = MACH_MSG_PORT_DESCRIPTOR;
          append(out, p);
          break;
        }
        case kOolDescriptor:
        case kOolVolatile: {
          if (second && !mem_.range_ok(first, second, GuestMemory::kRead)) return MACH_SEND_INVALID_MEMORY;
          mach_msg_ool_descriptor_t o = {};
          o.address = second ? mem_.host(first) : nullptr;
          o.size = second;
          o.copy = MACH_MSG_VIRTUAL_COPY;
          o.type = MACH_MSG_OOL_DESCRIPTOR;
          carried.push_back({out.size(), o.address, false});
          append(out, o);
          if (d[8] && second) given_away.push_back({first, second});
          break;
        }
        case kOolPortsDescriptor: {
          if (second > 0x100000 || (second && !mem_.range_ok(first, 4ull * second, GuestMemory::kRead))) {
            return MACH_SEND_INVALID_MEMORY;
          }
          std::vector<mach_port_t> names(second);
          for (u32 k = 0; k < second; k++) names[k] = mem_.r32(first + 4 * k);
          name_arrays.push_back(std::move(names));
          mach_msg_ool_ports_descriptor_t o = {};
          o.address = second ? name_arrays.back().data() : nullptr;
          o.count = second;
          o.copy = MACH_MSG_VIRTUAL_COPY;
          o.disposition = d[10];
          o.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
          carried.push_back({out.size(), o.address, true});
          append(out, o);
          if (d[8] && second) given_away.push_back({first, 4 * second});
          break;
        }
        default:
          return MACH_SEND_INVALID_TYPE;
      }
    }
  } else {
    append(out, body);
  }

  const tunnel::Meta meta = {tunnel::kMagic,
                             (be ? tunnel::kBigEndian : 0) | (opt_.as_root ? tunnel::kFromRoot : 0),
                             size, 0};
  append(out, meta);
  out.insert(out.end(), g, g + size);
  out.resize((out.size() + 3) & ~(size_t)3);

  auto* hp = (mach_msg_header_t*)out.data();
  hp->msgh_size = (mach_msg_size_t)out.size();
  const bool timed = option & MACH_SEND_TIMEOUT;
  const kern_return_t kr =
      ::mach_msg(hp, MACH_SEND_MSG | MACH_SEND_INTERRUPT | (timed ? MACH_SEND_TIMEOUT : 0),
                 hp->msgh_size, 0, MACH_PORT_NULL, timed ? timeout : MACH_MSG_TIMEOUT_NONE,
                 MACH_PORT_NULL);
  if (kr == KERN_SUCCESS) {
    for (const Range& r : given_away) mem_.unmap(r.addr, r.size);
  } else {
    // A message that was taken in but then not delivered (it timed out, or
    // a signal cut in) is handed back to this process, as if received: its
    // out-of-line memory comes back as fresh copies, at new addresses
    // written into `out`. The guest never sees them, so they go now.
    for (const Carried& c : carried) {
      if (c.ports) {
        mach_msg_ool_ports_descriptor_t o;
        memcpy(&o, out.data() + c.at, sizeof o);
        if (o.address && o.address != c.address) {
          mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)o.address,
                             (mach_vm_size_t)o.count * sizeof(mach_port_t));
        }
      } else {
        mach_msg_ool_descriptor_t o;
        memcpy(&o, out.data() + c.at, sizeof o);
        if (o.address && o.address != c.address) {
          mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)o.address, o.size);
        }
      }
    }
  }
  return (u32)kr;
}

// Copies out-of-line memory the host kernel delivered somewhere in this
// process into fresh guest memory, where the guest can reach it.
u32 Process::place_out_of_line(u64 host_address, u64 size) {
  if (!size) return 0;
  const u64 len = (size + 0xFFF) & ~0xFFFull;
  u32 at = 0;
  if (len < 0xC0000000ull) at = mem_.find_free(len, 0x1000, 0xC0000000u);
  if (at && mem_.map(at, len, GuestMemory::kRead | GuestMemory::kWrite)) {
    memcpy(mem_.host(at), (const void*)host_address, size);
  } else {
    log_warn("no room in the guest for %llu bytes that came with a Mach message",
             (unsigned long long)size);
    at = 0;
  }
  mach_vm_deallocate(mach_task_self(), host_address, size);
  return at;
}

bool Process::ipc_decode(const void* host_message, Incoming& in) {
  const bool be = mem_.big_endian();
  const auto* h = (const mach_msg_header_t*)host_message;
  const u8* const start = (const u8*)host_message;
  const u8* const end = start + h->msgh_size;

  const auto* trailer = (const mach_msg_audit_trailer_t*)(start + round_msg(h->msgh_size));
  in.port = h->msgh_local_port;
  in.seqno = trailer->msgh_seqno;
  in.sender[0] = trailer->msgh_sender.val[0];
  in.sender[1] = trailer->msgh_sender.val[1];
  for (int i = 0; i < 8; i++) in.audit[i] = trailer->msgh_audit.val[i];

  // What the host kernel handed over with the message.
  struct Carried {
    u8 type;
    u8 disposition;
    u32 name;      // for a port
    u64 address;   // for memory or an array of ports
    u32 count;     // bytes, or ports
  };
  std::vector<Carried> carried;
  const u8* p = start + sizeof(mach_msg_header_t);
  const bool host_complex = h->msgh_bits & MACH_MSGH_BITS_COMPLEX;
  if (host_complex) {
    const u32 count = ((const mach_msg_body_t*)p)->msgh_descriptor_count;
    p += sizeof(mach_msg_body_t);
    for (u32 i = 0; i < count && p < end; i++) {
      const auto* d = (const mach_msg_descriptor_t*)p;
      Carried c = {};
      switch (d->type.type) {
        case MACH_MSG_PORT_DESCRIPTOR:
          c = {kPortDescriptor, (u8)d->port.disposition, d->port.name, 0, 0};
          p += sizeof(mach_msg_port_descriptor_t);
          break;
        case MACH_MSG_OOL_DESCRIPTOR:
        case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
          c = {kOolDescriptor, 0, 0, (u64)(uintptr_t)d->out_of_line.address, d->out_of_line.size};
          p += sizeof(mach_msg_ool_descriptor_t);
          break;
        case MACH_MSG_OOL_PORTS_DESCRIPTOR:
          c = {kOolPortsDescriptor, (u8)d->ool_ports.disposition, 0,
               (u64)(uintptr_t)d->ool_ports.address, d->ool_ports.count};
          p += sizeof(mach_msg_ool_ports_descriptor_t);
          break;
        default:
          log_warn("a Mach message arrived with a kind of descriptor (%u) cocaine doesn't know",
                   (unsigned)d->type.type);
          return false;
      }
      carried.push_back(c);
    }
  }

  // Writes one guest descriptor for what was carried, giving memory and
  // arrays a home in the guest.
  auto write_descriptor = [&](u8* d, const Carried& c) {
    memset(d, 0, kGuestDescriptor);
    d[11] = c.type;
    if (c.type == kPortDescriptor) {
      wr(d, c.name, be);
      d[10] = c.disposition;
    } else if (c.type == kOolDescriptor) {
      wr(d, place_out_of_line(c.address, c.count), be);
      wr(d + 4, c.count, be);
      d[9] = 1;  // a copy
    } else {
      // An array of port names, each four bytes in the guest's byte order.
      u32 at = 0;
      if (c.count) {
        const u64 len = (4ull * c.count + 0xFFF) & ~0xFFFull;
        at = mem_.find_free(len, 0x1000, 0xC0000000u);
        if (at && mem_.map(at, len, GuestMemory::kRead | GuestMemory::kWrite)) {
          const auto* names = (const mach_port_t*)(uintptr_t)c.address;
          for (u32 k = 0; k < c.count; k++) mem_.w32(at + 4 * k, names[k]);
        } else {
          at = 0;
        }
        mach_vm_deallocate(mach_task_self(), c.address, c.count * sizeof(mach_port_t));
      }
      wr(d, at, be);
      wr(d + 4, c.count, be);
      d[9] = 1;
      d[10] = c.disposition;
    }
  };
  auto write_header = [&](u8* o, bool complex, u32 size) {
    wr(o, (h->msgh_bits & 0x1F1F) | (complex ? kComplex : 0), be);
    wr(o + 4, size, be);
    wr(o + 8, h->msgh_remote_port, be);
    wr(o + 12, h->msgh_local_port, be);
    wr(o + 16, 0, be);
    wr(o + 20, (u32)h->msgh_id, be);
  };

  const auto* meta = (const tunnel::Meta*)p;
  const bool tunneled = host_complex && p + sizeof(tunnel::Meta) <= end &&
                        meta->magic == tunnel::kMagic && meta->size >= kGuestHeader &&
                        p + sizeof(tunnel::Meta) + meta->size <= end;
  if (tunneled) {
    // From another guest (or this one): its message, with the fields the
    // kernel fills in for a receiver rewritten. The sender may be the other
    // kind of CPU, so its header and descriptors are read in its byte order;
    // the rest of the body is the two programs' own business.
    const bool sender_be = meta->flags & tunnel::kBigEndian;
    if (meta->flags & tunnel::kFromRoot) {
      in.sender[0] = in.sender[1] = 0;                 // user and group
      for (int i = 1; i <= 4; i++) in.audit[i] = 0;    // effective and real user and group
    }
    const u8* g = p + sizeof(tunnel::Meta);
    in.bytes.assign(g, g + meta->size);
    u8* o = in.bytes.data();
    const bool complex = rd(g, sender_be) & kComplex;
    write_header(o, complex, meta->size);
    const u32 count = complex && meta->size >= kGuestHeader + 4 ? rd(g + kGuestHeader, sender_be) : 0;
    if (count != carried.size() ||
        (complex && (u64)kGuestHeader + 4 + (u64)kGuestDescriptor * count > meta->size)) {
      log_warn("a Mach message arrived whose contents don't match its description; dropped");
      return false;
    }
    if (complex) wr(o + kGuestHeader, count, be);
    for (u32 i = 0; i < count; i++) {
      write_descriptor(o + kGuestHeader + 4 + kGuestDescriptor * i, carried[i]);
    }
    return true;
  }

  // From this Mac's own kernel: the notices a program asks for about its
  // ports (a port died, the last sender went away), or an error reply. These
  // have the same shape on both systems apart from byte order: the header,
  // any descriptors, a data-representation record, then 32-bit values.
  const size_t rest = p < end ? (size_t)(end - p) : 0;
  in.bytes.assign(kGuestHeader + (host_complex ? 4 + kGuestDescriptor * carried.size() : 0) + rest, 0);
  u8* o = in.bytes.data();
  write_header(o, host_complex, (u32)in.bytes.size());
  u8* q = o + kGuestHeader;
  if (host_complex) {
    wr(q, (u32)carried.size(), be);
    q += 4;
    for (const Carried& c : carried) {
      write_descriptor(q, c);
      q += kGuestDescriptor;
    }
  }
  memcpy(q, p, rest);
  if (rest >= 8) {
    static const u8 kBig[8] = {0, 0, 0, 0, 0, 0, 0, 0}, kLittle[8] = {0, 0, 0, 0, 1, 0, 0, 0};
    memcpy(q, be ? kBig : kLittle, 8);
    if (be) {
      for (size_t k = 8; k + 4 <= rest; k += 4) wr(q + k, rd(p + k, false), true);
    }
  }
  return true;
}

// Drops a message the guest will never see, along with what it carried.
void Process::ipc_discard(const Incoming& in) {
  const bool be = mem_.big_endian();
  const u8* g = in.bytes.data();
  const u32 size = (u32)in.bytes.size();
  if (size < kGuestHeader) return;
  const mach_port_t self = mach_task_self();
  if (const u32 reply = rd(g + 8, be)) mach_port_deallocate(self, reply);
  if (!(rd(g, be) & kComplex) || size < kGuestHeader + 4) return;
  const u32 count = rd(g + kGuestHeader, be);
  for (u32 i = 0; i < count && kGuestHeader + 4 + kGuestDescriptor * (i + 1) <= size; i++) {
    const u8* d = g + kGuestHeader + 4 + kGuestDescriptor * i;
    const u32 first = rd(d, be), second = rd(d + 4, be);
    if (d[11] == kPortDescriptor) {
      if (d[10] == MACH_MSG_TYPE_MOVE_RECEIVE) {
        mach_port_mod_refs(self, first, MACH_PORT_RIGHT_RECEIVE, -1);
      } else if (first) {
        mach_port_deallocate(self, first);
      }
    } else if (first) {
      mem_.unmap(first, d[11] == kOolPortsDescriptor ? 4 * second : second);
    }
  }
}

// Puts a received message into the guest's buffer, followed by the trailer:
// the kernel's own notes about the message, as much of them as was asked for.
u32 Process::ipc_deliver(Incoming&& in, u32 buf, u32 option, u32 rcv_size) {
  const u32 size = (u32)in.bytes.size();
  const u32 padded = (size + 3) & ~3u;
  const u32 elements = (option >> 24) & 0xF;
  const u32 trailer = elements == 0 ? 8 : elements == 1 ? 12 : elements == 2 ? 20 : 52;
  if ((u64)padded + trailer > rcv_size) {
    if (option & MACH_RCV_LARGE) {
      // The guest is told how much room it needs and will ask again; a real
      // kernel would have left the message on the port meanwhile.
      if (mem_.range_ok(buf, 8, GuestMemory::kWrite)) mem_.w32(buf + 4, size);
      std::lock_guard<std::mutex> lk(kmu_);
      stash_[in.port].push_front(std::move(in));
    } else {
      ipc_discard(in);
    }
    return MACH_RCV_TOO_LARGE;
  }
  if (!mem_.range_ok(buf, (u64)padded + trailer, GuestMemory::kWrite)) {
    ipc_discard(in);
    return MACH_RCV_INVALID_DATA;
  }
  memset(mem_.host(buf), 0, padded + trailer);
  memcpy(mem_.host(buf), in.bytes.data(), size);
  const u32 t = buf + padded;
  mem_.w32(t + 4, trailer);
  if (trailer >= 12) mem_.w32(t + 8, in.seqno);
  if (trailer >= 20) {
    mem_.w32(t + 12, in.sender[0]);
    mem_.w32(t + 16, in.sender[1]);
  }
  if (trailer >= 52) {
    for (u32 i = 0; i < 8; i++) mem_.w32(t + 20 + 4 * i, in.audit[i]);
  }
  return KERN_SUCCESS;
}

u32 Process::ipc_receive(u32 buf, u32 option, u32 rcv_size, u32 rcv_name, u32 timeout) {
  const mach_port_t self = mach_task_self();
  {
    // First anything kept back from an earlier, too-small receive: on this
    // port, or on a member of this port set.
    std::unique_lock<std::mutex> lk(kmu_);
    if (!stash_.empty()) {
      auto it = stash_.find(rcv_name);
      if (it == stash_.end()) {
        mach_port_name_array_t members = nullptr;
        mach_msg_type_number_t n = 0;
        if (mach_port_get_set_status(self, rcv_name, &members, &n) == KERN_SUCCESS) {
          for (u32 i = 0; i < n && it == stash_.end(); i++) it = stash_.find(members[i]);
          mach_vm_deallocate(self, (mach_vm_address_t)members, n * sizeof(mach_port_name_t));
        }
      }
      if (it != stash_.end()) {
        Incoming in = std::move(it->second.front());
        it->second.pop_front();
        if (it->second.empty()) stash_.erase(it);
        lk.unlock();
        return ipc_deliver(std::move(in), buf, option, rcv_size);
      }
    }
  }

  static thread_local std::vector<u8> space(16384);
  const bool timed = option & MACH_RCV_TIMEOUT;
  for (;;) {
    auto* h = (mach_msg_header_t*)space.data();
    // Interruptible, so a signal's handler can cut in; the guest's C library
    // comes back to the receive afterwards.
    const kern_return_t kr = ::mach_msg(
        h,
        MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_INTERRUPT | (timed ? MACH_RCV_TIMEOUT : 0) |
            MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
            MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
        0, (mach_msg_size_t)space.size(), rcv_name, timed ? timeout : MACH_MSG_TIMEOUT_NONE,
        MACH_PORT_NULL);
    if (kr == MACH_RCV_TOO_LARGE) {
      space.resize((size_t)h->msgh_size + 256);
      continue;
    }
    if (kr != KERN_SUCCESS) return (u32)kr;
    Incoming in;
    if (!ipc_decode(h, in)) continue;
    return ipc_deliver(std::move(in), buf, option, rcv_size);
  }
}

// A session's first process starts with a bootstrap port of its own that
// nobody answers. That costs nothing, and most command line programs never
// need more. When one does (it sends launchd a message, or makes a child
// that might), the session server is started and given the port: everything
// already sent to it is still queued, and every right to it stays good.
//
// The session also gets a name here, held with this Mac's own launchd, by
// which the session's other processes find the server.
void Process::ensure_session_server() {
  std::lock_guard<std::mutex> lk(session_mu_);
  if (!owns_bootstrap_) return;
  owns_bootstrap_ = false;
  if (opt_.self_path.empty()) return;

  const std::string name = "cocaine.session." + std::to_string(getpid()) + "." +
                           std::to_string(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) % 1000000007ull);
  mach_port_t named = MACH_PORT_NULL;
  kern_return_t kr = bootstrap_check_in(bootstrap_port, name.c_str(), &named);
  if (kr != KERN_SUCCESS) {
    log_warn("could not name a session (%s); system services will not answer",
             bootstrap_strerror(kr));
    return;
  }

  std::string pacing_error;
  uint32_t* pacing = create_session_pacing(name, initial_low_latency_pacing(), pacing_error);
  if (!pacing) log_warn("low latency menu unavailable: %s", pacing_error.c_str());

  const std::string root = std::to_string(getpid());
  std::vector<std::string> cmd = {opt_.self_path, "--session-server", root, "--session", name};
  if (!opt_.sysroot.empty()) {
    cmd.push_back("--sysroot");
    cmd.push_back(opt_.sysroot);
  }
  // The daemons will run on the same kind of CPU as this program, which
  // decides which of them can be used at all; and for a program with windows
  // the window server is started straight away, as it would be long before
  // any application on a real Mac, rather than when first asked for.
  cmd.push_back("--prefer-arch");
  cmd.push_back(mem_.big_endian() ? "ppc" : "i386");
  if (has_windows_) cmd.push_back("--windows");
  if (g_verbosity > 0) cmd.push_back(g_verbosity > 1 ? "-vv" : "-v");
  std::vector<char*> argv;
  for (std::string& s : cmd) argv.push_back(s.data());
  argv.push_back(nullptr);

  // Forked twice, so the server is nobody's child: a guest that waits for
  // "any child" must not find it.
  const pid_t pid = fork();
  if (pid == 0) {
    if (fork() == 0) {
      execve(opt_.self_path.c_str(), argv.data(), environ);
      _exit(127);
    }
    _exit(0);
  }
  if (pid > 0) {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }

  // The server looks the name up, asks, and is sent both ports to receive
  // on: the guests' bootstrap port and the named one.
  struct {
    mach_msg_header_t h;
    u8 trailer[64];
  } request = {};
  kr = pid < 0 ? KERN_FAILURE
               : ::mach_msg(&request.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof request, named,
                            10000, MACH_PORT_NULL);
  if (kr == KERN_SUCCESS && request.h.msgh_id == tunnel::kHandoffRequest) {
    struct {
      mach_msg_header_t h;
      mach_msg_body_t body;
      mach_msg_port_descriptor_t ports[3];
    } reply = {};
    reply.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    reply.h.msgh_size = sizeof reply;
    reply.h.msgh_remote_port = request.h.msgh_remote_port;
    reply.h.msgh_id = tunnel::kHandoffReply;
    reply.body.msgh_descriptor_count = 3;
    reply.ports[0].name = bootstrap_port_;
    reply.ports[1].name = named;
    reply.ports[2].name = task_port_;
    reply.ports[0].disposition = reply.ports[1].disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
    reply.ports[2].disposition = MACH_MSG_TYPE_COPY_SEND;
    for (auto& p : reply.ports) p.type = MACH_MSG_PORT_DESCRIPTOR;
    kr = ::mach_msg(&reply.h, MACH_SEND_MSG, sizeof reply, 0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  } else if (kr == KERN_SUCCESS) {
    kr = KERN_FAILURE;
  }
  if (kr != KERN_SUCCESS) {
    log_warn("could not start the session server (%s); system services will not answer",
             mach_error_string(kr));
    mach_port_mod_refs(mach_task_self(), named, MACH_PORT_RIGHT_RECEIVE, -1);
    if (pacing) {
      munmap(pacing, sizeof *pacing);
      const std::string path = session_pacing_path(name);
      unlink(path.c_str());
      rmdir(path.substr(0, path.rfind('/')).c_str());
    }
    return;
  }
  pacing_control_.store(pacing, std::memory_order_release);
  session_name_ = name;
  if (g_verbosity > 0) {
    char tmp[PATH_MAX] = "/tmp/";
    confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
    log_info("started this session's server; it and the daemons it runs log to %scocaine-session-%d.log",
             tmp, (int)getpid());
  }
}
