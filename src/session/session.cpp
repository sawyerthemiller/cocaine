// The session server: cocaine's stand-in for launchd.
//
// On a real Mac, launchd is the directory of system services. A program asks
// it for a service by name and gets a Mach port back; launchd starts the
// daemon behind that port the first time somebody actually sends to it.
// Daemons, in turn, "check in" with launchd to collect the ports they are
// supposed to serve.
//
// This is a native program that speaks that protocol to guest processes. It
// knows which daemon provides which service from the same files launchd reads
// (the job descriptions in the system root), and it runs Apple's own daemons
// from the system root, under cocaine, when they are first needed.
//
// One server runs per session: the tree of guest processes started by one
// run of cocaine. It is started by the session's first process when that
// process first needs it, and ends when that process does, taking the daemons
// it started with it.

#include "kernel/process.h"
#include "kernel/protect.h"
#include "session/session.h"
#include "session/art.h"

#include <libproc.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pwd.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <map>
#include <set>
#include <vector>

#include "kernel/tunnel.h"

extern char** environ;
extern "C" mach_port_name_t mach_reply_port(void);  // missing from the system headers

namespace {

// launchd's message numbers, as the 10.5 C library sends them.
enum : s32 {
  kCheckIn = 402,
  kRegister = 403,
  kLookUp = 404,
  kParent = 406,
  kPostForkPing = 407,
};
constexpr u32 kUnknownService = 1102, kServiceActive = 1103;
constexpr u32 kMigBadId = (u32)-303;
constexpr u32 kGuestComplex = 0x80000000u;
constexpr int kMaxLaunches = 3;

// The daemons known to work under cocaine. Others are left out, so that a
// program asking for their services is told "no such service" (which every
// program copes with) instead of waiting on a daemon that can't run yet.
// COCAINE_DAEMONS overrides: "all", "none", or a comma-separated list of job
// labels to use instead.
const char* const kWorkingDaemons[] = {
    "com.apple.notifyd",            // notifications between programs
    "com.apple.DirectoryServices",  // users and groups
    "com.apple.bsd.dirhelper",      // per-user temporary folders
    "com.apple.SecurityServer",     // securityd: security sessions, keychains
    "com.apple.coreservicesd",      // Launch Services: which apps exist and are running
    "com.apple.configd",            // system settings and their change notices (see kExtraArguments)
    "com.apple.pboard",             // pasteboards shared by guest programs
};
// Arguments added to a daemon's own. configd's plug-ins manage the Mac's
// real network interfaces, power and Bluetooth, none of which a guest
// should touch; -b runs it without them, serving settings and notices only.
// Apple's code assumes configd is always there: the Apple menu, for one,
// crashes in SCPreferences when it isn't.
const struct {
  const char* job;
  const char* argument;
} kExtraArguments[] = {
    {"com.apple.configd", "-b"},
    // configd stays in the foreground only for launchd, process 1, which
    // the session server isn't; otherwise it forks itself away.
    {"com.apple.configd", "-d"},
};

// Services a daemon is told don't exist. The directory daemon asks configd
// to register it while starting, and waits; configd, while starting, looks
// up a user, which means asking the directory daemon: neither would ever
// answer. On a real Mac configd is up long before the directory daemon. Told
// the directory daemon isn't there, configd's library reads /etc/passwd, as
// in single-user mode; configd only ever needs root.
const struct {
  const char* job;
  const char* service;
} kHiddenServices[] = {
    {"com.apple.configd", "com.apple.system.DirectoryService.libinfo_v1"},
};
// ... and the ones that put windows on the screen. The Intel halves of the
// font server and the login window are stored encrypted (see
// kernel/protect.h), so an Intel session has these only where the keys to
// them have been supplied.
const char* const kWindowDaemons[] = {
    "com.apple.WindowServer",  // draws every window, hands out mouse and keys
    "com.apple.ATSServer",     // fonts
    "com.apple.loginwindow",   // sets a login session up
    "com.apple.hidd",          // HID event service needed by later Leopard releases
};

// Services that no job description lists because the daemon publishes them
// itself once it is running. On a real Mac that daemon is started at boot,
// long before anyone asks; here it is started by the first request for one
// of these names, and the request waits until the daemon has published it.
const struct {
  const char* service;
  const char* job;
} kPublishedLater[] = {
    {"com.apple.windowserver.session", "com.apple.WindowServer"},
    {"com.apple.windowserver.active", "com.apple.WindowServer"},
};

struct Job {
  std::string label;
  std::vector<std::string> argv;  // guest paths
  std::vector<std::string> env;   // extra environment, as NAME=VALUE
  bool as_root = true;
  pid_t pid = 0;
  int launches = 0;
  int sock = -1;  // this end of the daemon's private line to "launchd"
};

struct Service {
  mach_port_t port = MACH_PORT_NULL;
  bool held = false;  // this server holds the receiving end (nobody has checked in)
  int job = -1;       // the job that provides it, if a job description names it
};

inline void wr(u8* p, u32 v, bool be) {
  if (be) v = __builtin_bswap32(v);
  memcpy(p, &v, 4);
}

std::string to_string(CFTypeRef v) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return "";
  char buf[2048];
  return CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8) ? buf : "";
}

class Server {
 public:
  Server(const std::string& self_path, const std::string& sysroot, pid_t root,
         const std::string& name, bool powerpc, bool windows)
      : self_path_(self_path), sysroot_(sysroot), name_(name), root_(root), windows_first_(windows),
        big_endian_(powerpc) {}

  int run();

 private:
  bool take_bootstrap_port();
  void add_current_user();
  void configure_window_server();
  void load_jobs(const std::string& guest_dir);
  void add_job(Job job, const std::vector<std::string>& services);
  void drain_requests();
  void drain_watched();
  void handle(mach_msg_header_t* h);
  void reap();
  void shut_down();
  void serve_socket(size_t job_index);
  void open_syslog();
  void read_syslog();

  bool make_port(Service& sv, const std::string& name);
  void watch(mach_port_t port, bool on);
  void launch(Job& job);
  void reply_code(mach_port_t to, s32 id, bool be, u32 code);
  void reply_port(mach_port_t to, s32 id, bool be, mach_port_t port, mach_msg_type_name_t how);
  void send(mach_port_t to, s32 id, bool be, const std::vector<u8>& guest, mach_port_t port,
            mach_msg_type_name_t how);

  std::string self_path_, sysroot_, name_;
  pid_t root_;
  // The guests' bootstrap port; the port new processes find by the session's
  // name; and where the kernel reports on service ports.
  mach_port_t bootstrap_ = MACH_PORT_NULL, named_ = MACH_PORT_NULL, notify_ = MACH_PORT_NULL;
  mach_port_t main_set_ = MACH_PORT_NULL, watch_set_ = MACH_PORT_NULL;
  std::vector<Job> jobs_;
  std::map<std::string, Service> services_;
  std::map<mach_port_t, std::string> by_port_;
  std::set<std::string> wanted_;
  // Lookups being held until a daemon publishes the name.
  struct Waiting {
    mach_port_t reply;
    s32 id;
    bool be;
  };
  std::map<std::string, std::vector<Waiting>> waiting_;
  // For memory shared by port: how far into its first host page each
  // entry's range starts (see tunnel.h).
  std::map<mach_port_t, u32> entry_offsets_;
  std::map<pid_t, mach_port_t> tasks_;
  void register_task(pid_t pid, mach_port_t port);
  mach_port_t task_for_pid(pid_t pid);
  // On a real Mac the window server is up, and has set up the login session,
  // before any app runs: apps find the security daemon and Launch Services
  // as that session presents them. Here the window server is started by the
  // session's first request, so until it has published its ports, lookups by
  // anything but the daemons are held back, and answered in order after.
  //
  // That is for a session whose first program has windows. Any other session
  // leaves the window server alone until something asks for it by name, and
  // holds lookups back from then.
  bool windows_first_ = false;
  bool gui_started_ = false, gate_ = false;
  void start_window_server();
  // ... and until the login window, started after it, says it is up: an app
  // that got in first would find itself behind the login window.
  bool login_pending_ = false;
  static constexpr u64 kGateTimer = 1;
  static constexpr u64 kPasteboardTimer = 2;
  std::vector<std::vector<u8>> held_;
  void open_gate();
  int job_of(pid_t pid);
  void answer_waiting(const std::string& name, mach_port_t port);
  bool big_endian_ = false;  // the kind of CPU the session's programs run on
  bool told_no_windows_ = false;
  std::string tmp_, log_path_;
  int kq_ = -1;
  int syslog_ = -1;
  std::vector<pid_t> started_;  // the process group of every daemon started
};

// The session's first process holds both of the session's ports until now. It
// is found by the session's name and asked for them.
bool Server::take_bootstrap_port() {
  const mach_port_t self = mach_task_self();
  mach_port_t first = MACH_PORT_NULL;
  kern_return_t kr = bootstrap_look_up(bootstrap_port, name_.c_str(), &first);
  if (kr != KERN_SUCCESS) {
    log_debug("looking up %s: %s", name_.c_str(), bootstrap_strerror(kr));
    return false;
  }
  struct {
    mach_msg_header_t h;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t ports[3];
    u8 trailer[64];
  } m = {};
  m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
  m.h.msgh_size = sizeof(mach_msg_header_t);
  m.h.msgh_remote_port = first;
  m.h.msgh_local_port = mach_reply_port();
  m.h.msgh_id = tunnel::kHandoffRequest;
  kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, m.h.msgh_size, sizeof m,
                m.h.msgh_local_port, 10000, MACH_PORT_NULL);
  mach_port_deallocate(self, first);
  log_debug("asked for the session's ports: result 0x%x, reply %d", kr, m.h.msgh_id);
  if (kr != KERN_SUCCESS || m.h.msgh_id != tunnel::kHandoffReply ||
      !(m.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) || m.body.msgh_descriptor_count != 3) {
    return false;
  }
  bootstrap_ = m.ports[0].name;
  named_ = m.ports[1].name;
  register_task(root_, m.ports[2].name);
  // Rights to send to the bootstrap port are what this server hands out.
  mach_port_insert_right(self, bootstrap_, bootstrap_, MACH_MSG_TYPE_MAKE_SEND);
  return true;
}

void Server::register_task(pid_t pid, mach_port_t port) {
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    mach_port_type_t type = 0;
    if (it->first == pid || mach_port_type(mach_task_self(), it->second, &type) != KERN_SUCCESS ||
        !(type & MACH_PORT_TYPE_SEND)) {
      mach_port_deallocate(mach_task_self(), it->second);
      it = tasks_.erase(it);
    } else {
      ++it;
    }
  }
  tasks_[pid] = port;
}

mach_port_t Server::task_for_pid(pid_t pid) {
  const auto it = tasks_.find(pid);
  if (it == tasks_.end()) return MACH_PORT_NULL;
  mach_port_type_t type = 0;
  if (mach_port_type(mach_task_self(), it->second, &type) == KERN_SUCCESS &&
      (type & MACH_PORT_TYPE_SEND)) return it->second;
  mach_port_deallocate(mach_task_self(), it->second);
  tasks_.erase(it);
  return MACH_PORT_NULL;
}

// The guest system's directory of users is the one in the system root, which
// has never heard of the person running cocaine. Its directory daemon keeps
// one small file per user; this adds the file for that person, with their
// real name, home and numbers, so that the guest system knows them.
// Tells the guest's window server not to hold screen updates back.
//
// Leopard's window server saves each update up for the display's next
// refresh, and where a display can't say when that is, for an eighth of a
// second: with the display it makes for itself here, every press of a button
// took that long to show. The setting below (the one Apple's own Quartz Debug
// flips, kept in the system root's preferences) has it draw at once. A value
// someone has put there on purpose is left alone.
// Sets one top-level value in a guest preferences file, keeping the rest.
static void set_guest_pref(const std::string& path, CFStringRef key, CFPropertyListRef value) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(),
                                                         (CFIndex)path.size(), false);
  if (!url) return;
  CFMutableDictionaryRef prefs = nullptr;
  if (CFReadStreamRef in = CFReadStreamCreateWithFile(nullptr, url)) {
    if (CFReadStreamOpen(in)) {
      CFPropertyListRef have = CFPropertyListCreateWithStream(nullptr, in, 0, kCFPropertyListMutableContainers,
                                                              nullptr, nullptr);
      CFReadStreamClose(in);
      if (have && CFGetTypeID(have) == CFDictionaryGetTypeID()) {
        prefs = (CFMutableDictionaryRef)have;
      } else if (have) {
        CFRelease(have);
      }
    }
    CFRelease(in);
  }
  if (!prefs) {
    prefs = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  }
  const void* was = CFDictionaryGetValue(prefs, key);
  if (!was || !CFEqual(was, value)) {
    CFDictionarySetValue(prefs, key, value);
    if (CFWriteStreamRef out = CFWriteStreamCreateWithFile(nullptr, url)) {
      if (CFWriteStreamOpen(out)) {
        CFPropertyListWrite(prefs, out, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
        CFWriteStreamClose(out);
      }
      CFRelease(out);
    }
  }
  CFRelease(prefs);
  CFRelease(url);
}

void Server::configure_window_server() {
  prepare_menu_logo(sysroot_);
  // Keep Leopard's point and pixel coordinates identical by default.
  // Its unfinished resolution independence remains an explicit opt-in.
  {
    double scale = 1.0;
    if (const char* e = getenv("COCAINE_UI_SCALE")) {
      const double v = atof(e);
      if (v >= 1.0 && v <= 3.0) scale = v;
    }
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &scale);
    set_guest_pref(sysroot_ + "/Library/Preferences/.GlobalPreferences.plist", CFSTR("AppleDisplayScaleFactor"), n);
    CFRelease(n);
  }
  const std::string path = sysroot_ + "/Library/Preferences/com.apple.windowserver.plist";
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(),
                                                         (CFIndex)path.size(), false);
  if (!url) return;
  CFMutableDictionaryRef prefs = nullptr;
  if (CFReadStreamRef in = CFReadStreamCreateWithFile(nullptr, url)) {
    if (CFReadStreamOpen(in)) {
      CFPropertyListRef have = CFPropertyListCreateWithStream(nullptr, in, 0, kCFPropertyListMutableContainers,
                                                              nullptr, nullptr);
      CFReadStreamClose(in);
      if (have && CFGetTypeID(have) == CFDictionaryGetTypeID()) {
        prefs = (CFMutableDictionaryRef)have;
      } else if (have) {
        CFRelease(have);
      }
    }
    CFRelease(in);
  }
  if (!prefs) {
    prefs = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  }
  CFMutableDictionaryRef compositor = nullptr;
  const void* have = CFDictionaryGetValue(prefs, CFSTR("Compositor"));
  if (have && CFGetTypeID(have) == CFDictionaryGetTypeID()) {
    compositor = (CFMutableDictionaryRef)have;
    CFRetain(compositor);
  } else {
    compositor = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(prefs, CFSTR("Compositor"), compositor);
  }
  if (!CFDictionaryContainsKey(compositor, CFSTR("deferredUpdates"))) {
    const int off = 0;
    CFNumberRef zero = CFNumberCreate(nullptr, kCFNumberIntType, &off);
    CFDictionarySetValue(compositor, CFSTR("deferredUpdates"), zero);
    CFRelease(zero);
    if (CFWriteStreamRef out = CFWriteStreamCreateWithFile(nullptr, url)) {
      if (CFWriteStreamOpen(out)) {
        CFPropertyListWrite(prefs, out, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
        CFWriteStreamClose(out);
      }
      CFRelease(out);
    }
  }
  CFRelease(compositor);
  CFRelease(prefs);
  CFRelease(url);
}

void Server::add_current_user() {
  const struct passwd* me = getpwuid(getuid());
  if (!me || !me->pw_name || getuid() == 0) return;
  const std::string local = sysroot_ + "/private/var/db/dslocal";
  const std::string file = local + "/nodes/Default/users/" + me->pw_name + ".plist";
  if (access(file.c_str(), F_OK) == 0 || access((local + "/nodes/Default/users").c_str(), W_OK) != 0) {
    return;
  }
  auto escaped = [](const char* s) {
    std::string out;
    for (; s && *s; s++) {
      if (*s == '&') out += "&amp;";
      else if (*s == '<') out += "&lt;";
      else if (*s == '>') out += "&gt;";
      else out += *s;
    }
    return out;
  };
  char uuid[40];
  std::snprintf(uuid, sizeof uuid, "C0CA1E00-0000-0000-0000-%012X", (unsigned)getuid());
  const std::pair<const char*, std::string> fields[] = {
      {"generateduid", uuid},
      {"gid", std::to_string(me->pw_gid)},
      {"home", escaped(me->pw_dir)},
      {"name", escaped(me->pw_name)},
      {"passwd", "*"},
      {"realname", escaped(me->pw_gecos && *me->pw_gecos ? me->pw_gecos : me->pw_name)},
      {"shell", "/bin/bash"},
      {"uid", std::to_string(getuid())},
  };
  std::string text =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n<dict>\n";
  for (const auto& f : fields) {
    text += std::string("\t<key>") + f.first + "</key>\n\t<array>\n\t\t<string>" + f.second +
            "</string>\n\t</array>\n";
  }
  text += "</dict>\n</plist>\n";
  FILE* out = fopen(file.c_str(), "w");
  if (!out) return;
  fwrite(text.data(), 1, text.size(), out);
  fclose(out);
  // The daemon's index of these files is rebuilt when it is missing.
  unlink((local + "/indices/Default/index").c_str());
  unlink((local + "/indices/Default/index-journal").c_str());
  log_info("added %s to the guest system's users", me->pw_name);
}

void Server::add_job(Job job, const std::vector<std::string>& services) {
  if (job.argv.empty()) return;
  const std::string host = sysroot_ + job.argv[0];
  if (access(host.c_str(), X_OK) != 0) return;
  const int index = (int)jobs_.size();
  jobs_.push_back(std::move(job));
  for (const std::string& name : services) services_[name].job = index;
}

// Reads the job descriptions launchd would: property lists naming a program
// and the Mach services it provides.
void Server::load_jobs(const std::string& guest_dir) {
  const std::string dir = sysroot_ + guest_dir;
  DIR* d = opendir(dir.c_str());
  if (!d) return;

  std::set<std::string> allowed;
  bool all = false;
  const char* choice = getenv("COCAINE_DAEMONS");
  if (!choice) {
    for (const char* label : kWorkingDaemons) allowed.insert(label);
    if (big_endian_ || have_protection_keys(sysroot_)) {
      for (const char* label : kWindowDaemons) allowed.insert(label);
    }
  } else if (!strcmp(choice, "all")) {
    all = true;
  } else if (strcmp(choice, "none") != 0) {
    std::string item;
    for (const char* c = choice;; c++) {
      if (*c == ',' || !*c) {
        if (!item.empty()) allowed.insert(item);
        item.clear();
        if (!*c) break;
      } else {
        item += *c;
      }
    }
  }

  while (const dirent* e = readdir(d)) {
    const std::string file = e->d_name;
    if (file.size() < 7 || file.compare(file.size() - 6, 6, ".plist") != 0) continue;
    const std::string path = dir + "/" + file;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(),
                                                           (CFIndex)path.size(), false);
    CFReadStreamRef stream = url ? CFReadStreamCreateWithFile(nullptr, url) : nullptr;
    CFPropertyListRef plist = nullptr;
    if (stream && CFReadStreamOpen(stream)) {
      plist = CFPropertyListCreateWithStream(nullptr, stream, 0, kCFPropertyListImmutable, nullptr,
                                             nullptr);
      CFReadStreamClose(stream);
    }
    if (stream) CFRelease(stream);
    if (url) CFRelease(url);
    if (!plist) continue;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
      CFDictionaryRef dict = (CFDictionaryRef)plist;
      Job job;
      job.label = to_string(CFDictionaryGetValue(dict, CFSTR("Label")));
      job.as_root = !CFDictionaryContainsKey(dict, CFSTR("UserName"));
      const void* args = CFDictionaryGetValue(dict, CFSTR("ProgramArguments"));
      if (args && CFGetTypeID(args) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount((CFArrayRef)args); i++) {
          job.argv.push_back(to_string(CFArrayGetValueAtIndex((CFArrayRef)args, i)));
        }
      }
      const void* env = CFDictionaryGetValue(dict, CFSTR("EnvironmentVariables"));
      if (env && CFGetTypeID(env) == CFDictionaryGetTypeID()) {
        const CFIndex n = CFDictionaryGetCount((CFDictionaryRef)env);
        std::vector<const void*> keys((size_t)n), values((size_t)n);
        CFDictionaryGetKeysAndValues((CFDictionaryRef)env, keys.data(), values.data());
        for (CFIndex i = 0; i < n; i++) {
          job.env.push_back(to_string(keys[(size_t)i]) + "=" + to_string(values[(size_t)i]));
        }
      }
      const std::string program = to_string(CFDictionaryGetValue(dict, CFSTR("Program")));
      if (!program.empty()) {
        if (job.argv.empty()) job.argv.push_back(program);
        job.argv[0] = program;
      }
      std::vector<std::string> names;
      const void* ms = CFDictionaryGetValue(dict, CFSTR("MachServices"));
      if (ms && CFGetTypeID(ms) == CFDictionaryGetTypeID()) {
        const CFIndex n = CFDictionaryGetCount((CFDictionaryRef)ms);
        std::vector<const void*> keys((size_t)n);
        CFDictionaryGetKeysAndValues((CFDictionaryRef)ms, keys.data(), nullptr);
        for (const void* k : keys) names.push_back(to_string(k));
      }
      for (const auto& extra : kExtraArguments) {
        if (job.label == extra.job && !job.argv.empty()) job.argv.push_back(extra.argument);
      }
      // A job with no services of its own is only kept if asked for by name.
      if (allowed.count(job.label) || (all && !names.empty())) add_job(std::move(job), names);
    }
    CFRelease(plist);
  }
  closedir(d);
}

void Server::watch(mach_port_t port, bool on) {
  if (on) {
    mach_port_insert_member(mach_task_self(), port, watch_set_);
  } else {
    mach_port_extract_member(mach_task_self(), port, watch_set_);
  }
}

// Creates the port for a service. This server keeps its receiving end until
// the daemon checks in, and a right to send to it for good, so that the name
// can go on being handed to clients.
bool Server::make_port(Service& sv, const std::string& name) {
  const mach_port_t self = mach_task_self();
  if (mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &sv.port) != KERN_SUCCESS) return false;
  mach_port_insert_right(self, sv.port, sv.port, MACH_MSG_TYPE_MAKE_SEND);
  sv.held = true;
  by_port_[sv.port] = name;
  // If whoever takes the receiving end dies, the kernel sends it back here.
  mach_port_t previous = MACH_PORT_NULL;
  mach_port_request_notification(self, sv.port, MACH_NOTIFY_PORT_DESTROYED, 0, notify_,
                                 MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
  if (sv.job >= 0) watch(sv.port, true);
  return true;
}

void Server::launch(Job& job) {
  if (job.pid > 0) return;
  if (++job.launches > kMaxLaunches) return;
  std::vector<std::string> cmd = {self_path_, "--sysroot", sysroot_, "--prefer-arch",
                                  big_endian_ ? "ppc" : "i386"};
  if (job.as_root) cmd.push_back("--as-root");
  cmd.push_back("--session");
  cmd.push_back(name_);
  if (g_verbosity > 1) cmd.push_back("-v");
  if (getenv("COCAINE_DAEMON_STRACE")) cmd.push_back("--strace");  // for working on cocaine itself
  cmd.push_back("--");
  cmd.push_back(sysroot_ + job.argv[0]);
  for (size_t i = 1; i < job.argv.size(); i++) cmd.push_back(job.argv[i]);
  std::vector<char*> argv;
  for (std::string& s : cmd) argv.push_back(s.data());
  argv.push_back(nullptr);

  // launchd's other way in: besides its Mach port, every job it starts gets
  // a socket to it, named in the job's environment. Daemons written against
  // launchd's own library collect their ports by asking on it.
  int line[2] = {-1, -1};
  if (job.sock >= 0) close(job.sock);
  job.sock = -1;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, line) == 0) {
    job.sock = line[0];
    fcntl(job.sock, F_SETFD, FD_CLOEXEC);
  }
  std::vector<std::string> env_strings = job.env;
  env_strings.push_back("__LAUNCHD_FD=3");
  std::vector<char*> envp;
  for (char** e = environ; *e; e++) envp.push_back(*e);
  for (std::string& s : env_strings) envp.push_back(s.data());
  envp.push_back(nullptr);

  // A daemon has no terminal. What it prints goes to the session's log.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // And it starts in the guest's "/", as launchd starts daemons, not in the
  // folder cocaine was run from: a daemon or a helper it starts that writes
  // to a relative path (Launch Services' lssave, saving a database it found
  // no home for, writes "~") would otherwise leave files there.
  posix_spawn_file_actions_addchdir_np(&actions, sysroot_.c_str());
  if (line[1] >= 0) posix_spawn_file_actions_adddup2(&actions, line[1], 3);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                                   0600);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // Nothing else is passed on. And each daemon leads a process group of its
  // own, so that whatever it starts in turn can be ended along with it.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, self_path_.c_str(), &actions, &attr, argv.data(), envp.data());
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (line[1] >= 0) close(line[1]);
  if (job.sock >= 0 && rc == 0) {
    struct kevent64_s ev;
    EV_SET64(&ev, (u64)job.sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
             (u64)(&job - jobs_.data()) + 1, 0, 0);
    kevent64(kq_, &ev, 1, nullptr, 0, 0, nullptr);
  }
  if (rc != 0) {
    log_warn("could not start the daemon %s: %s", job.label.c_str(), strerror(rc));
    return;
  }
  job.pid = pid;
  started_.push_back(pid);
  log_info("started the daemon %s (process %d)", job.label.c_str(), (int)pid);
}

// Replies travel the way all guest messages do: a guest message inside a host
// one, with any port right as a host descriptor.
void Server::send(mach_port_t to, s32 id, bool be, const std::vector<u8>& guest, mach_port_t port,
                  mach_msg_type_name_t how) {
  if (!MACH_PORT_VALID(to)) return;
  thread_local std::vector<u8> out;
  const size_t head = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t);
  const size_t extra = (port ? sizeof(mach_msg_port_descriptor_t) : 0) + sizeof(tunnel::Meta) + guest.size();
  out.clear();
  out.reserve(head + extra + 3);
  out.resize(head, 0);
  auto add = [&](const void* p, size_t n) {
    out.insert(out.end(), (const u8*)p, (const u8*)p + n);
  };
  if (port) {
    mach_msg_port_descriptor_t d = {};
    d.name = port;
    d.disposition = how;
    d.type = MACH_MSG_PORT_DESCRIPTOR;
    add(&d, sizeof d);
  }
  // launchd is root's, and says so.
  const tunnel::Meta meta = {tunnel::kMagic, (be ? tunnel::kBigEndian : 0) | tunnel::kFromRoot,
                             (u32)guest.size(), 0};
  add(&meta, sizeof meta);
  add(guest.data(), guest.size());
  out.resize((out.size() + 3) & ~(size_t)3);

  auto* h = (mach_msg_header_t*)out.data();
  h->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
  h->msgh_size = (mach_msg_size_t)out.size();
  h->msgh_remote_port = to;
  h->msgh_id = id;
  ((mach_msg_body_t*)(h + 1))->msgh_descriptor_count = port ? 1 : 0;
  if (mach_msg(h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, h->msgh_size, 0, MACH_PORT_NULL, 1000,
               MACH_PORT_NULL) != KERN_SUCCESS) {
    mach_msg_destroy(h);
  }
}

void Server::reply_code(mach_port_t to, s32 id, bool be, u32 code) {
  std::vector<u8> g(36, 0);
  wr(&g[4], 36, be);
  wr(&g[20], (u32)id, be);
  g[28] = be ? 0 : 1;  // the data-representation record: which byte order follows
  wr(&g[32], code, be);
  send(to, id, be, g, MACH_PORT_NULL, 0);
}

void Server::reply_port(mach_port_t to, s32 id, bool be, mach_port_t port,
                        mach_msg_type_name_t how) {
  std::vector<u8> g(40, 0);
  wr(&g[0], kGuestComplex, be);
  wr(&g[4], 40, be);
  wr(&g[20], (u32)id, be);
  wr(&g[24], 1, be);  // one descriptor, which the receiving side fills in
  send(to, id, be, g, port, how);
}

// Answers the lookups that were waiting for `name`: with the port, or, if
// there is none, with "no such service".
void Server::answer_waiting(const std::string& name, mach_port_t port) {
  const auto it = waiting_.find(name);
  if (it == waiting_.end()) return;
  for (const Waiting& w : it->second) {
    if (port) {
      reply_port(w.reply, w.id, w.be, port, MACH_MSG_TYPE_COPY_SEND);
    } else {
      reply_code(w.reply, w.id, w.be, kUnknownService);
    }
  }
  waiting_.erase(it);
}

// Starts the window server, if this session has one, and from then holds
// lookups by anything but the daemons until the session is ready for
// applications (see open_gate's callers).
void Server::start_window_server() {
  if (gui_started_) return;
  gui_started_ = true;
  for (Job& job : jobs_) {
    if (job.label == "com.apple.WindowServer") {
      gate_ = true;
      launch(job);
      if (job.pid <= 0) gate_ = false;
    }
  }
  if (gate_) {
    // However the window server and the login window fare, nothing waits on
    // them for ever: after this long, programs are let in regardless.
    struct kevent64_s ev;
    EV_SET64(&ev, kGateTimer, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 25000, 0, 0, 0);
    kevent64(kq_, &ev, 1, nullptr, 0, 0, nullptr);
  }
}

// The job a process belongs to: one of the daemons started here, or anything
// such a daemon started, however far down. Most of those share the daemon's
// process group, but not all: the login window runs helpers in groups of
// their own and waits for them, so parentage is followed too. -1 if none.
int Server::job_of(pid_t pid) {
  for (int depth = 0; depth < 16 && pid > 1 && pid != root_; depth++) {
    const pid_t group = getpgid(pid);
    for (size_t i = 0; i < jobs_.size(); i++) {
      if (jobs_[i].pid > 0 && (jobs_[i].pid == pid || jobs_[i].pid == group)) return (int)i;
    }
    struct proc_bsdinfo info;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != (int)sizeof info) break;
    pid = (pid_t)info.pbi_ppid;
  }
  return -1;
}

void Server::open_gate() {
  if (!gate_) return;
  gate_ = false;
  std::vector<std::vector<u8>> held;
  held.swap(held_);
  for (std::vector<u8>& m : held) handle((mach_msg_header_t*)m.data());
}

void Server::handle(mach_msg_header_t* h) {
  const mach_port_t self = mach_task_self();
  const u8* const start = (const u8*)h;
  const u8* const end = start + h->msgh_size;
  const u8* p = start + sizeof(mach_msg_header_t);
  std::vector<mach_port_t> ports;
  const bool complex = h->msgh_bits & MACH_MSGH_BITS_COMPLEX;
  if (complex) {
    const u32 count = ((const mach_msg_body_t*)p)->msgh_descriptor_count;
    p += sizeof(mach_msg_body_t);
    for (u32 i = 0; i < count; i++) {
      const auto* d = (const mach_msg_descriptor_t*)p;
      if (d->type.type != MACH_MSG_PORT_DESCRIPTOR) {  // nothing launchd is ever sent
        mach_msg_destroy(h);
        return;
      }
      ports.push_back(d->port.name);
      p += sizeof(mach_msg_port_descriptor_t);
    }
  }
  const auto* meta = (const tunnel::Meta*)p;
  const bool tunneled = complex && p + sizeof(tunnel::Meta) <= end && meta->magic == tunnel::kMagic &&
                        meta->size >= 24 && p + sizeof(tunnel::Meta) + meta->size <= end;

  if (!tunneled) {
    if (h->msgh_id == tunnel::kEntryNote && ports.size() == 1 && p + 4 <= end) {
      entry_offsets_[ports[0]] = *(const u32*)p;  // the right is kept, to keep the name
      return;
    }
    if (h->msgh_id == tunnel::kEntryAsk && ports.size() == 1) {
      struct {
        mach_msg_header_t h;
        u32 offset;
      } answer = {};
      const auto it = entry_offsets_.find(ports[0]);
      answer.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
      answer.h.msgh_size = sizeof answer;
      answer.h.msgh_remote_port = h->msgh_remote_port;
      answer.h.msgh_id = tunnel::kEntryAnswer;
      answer.offset = it == entry_offsets_.end() ? 0 : it->second;
      if (MACH_PORT_VALID(ports[0])) mach_port_deallocate(self, ports[0]);
      if (mach_msg(&answer.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof answer, 0, MACH_PORT_NULL,
                   1000, MACH_PORT_NULL) != KERN_SUCCESS) {
        mach_msg_destroy(&answer.h);
      }
      return;
    }
    if (h->msgh_id == tunnel::kSessionReady) {
      if (login_pending_) log_info("the login window has the session ready; applications may come in");
      login_pending_ = false;
      open_gate();
      return;
    }
    if (h->msgh_id == tunnel::kTaskLookup && !complex && p + sizeof(u32) <= end &&
        MACH_PORT_VALID(h->msgh_remote_port)) {
      u32 pid;
      memcpy(&pid, p, sizeof pid);
      struct {
        mach_msg_header_t h;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t port;
      } answer = {};
      answer.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
      answer.h.msgh_size = sizeof answer;
      answer.h.msgh_remote_port = h->msgh_remote_port;
      answer.h.msgh_id = tunnel::kTaskLookupReply;
      answer.body.msgh_descriptor_count = 1;
      answer.port.name = task_for_pid((pid_t)pid);
      answer.port.disposition = MACH_MSG_TYPE_COPY_SEND;
      answer.port.type = MACH_MSG_PORT_DESCRIPTOR;
      if (mach_msg(&answer.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof answer, 0, MACH_PORT_NULL,
                   1000, MACH_PORT_NULL) != KERN_SUCCESS) {
        mach_msg_destroy(&answer.h);
      }
      return;
    }
    if (h->msgh_id == tunnel::kJoin) {
      // A new process of the session, asking for the bootstrap port.
      if (ports.size() != 1 || !MACH_PORT_VALID(ports[0]) || !MACH_PORT_VALID(h->msgh_remote_port)) {
        mach_msg_destroy(h);
        return;
      }
      const auto* trailer = (const mach_msg_audit_trailer_t*)(start + round_msg(h->msgh_size));
      register_task((pid_t)trailer->msgh_audit.val[5], ports[0]);
      struct {
        mach_msg_header_t h;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t port;
      } answer = {};
      answer.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
      answer.h.msgh_size = sizeof answer;
      answer.h.msgh_remote_port = h->msgh_remote_port;
      answer.h.msgh_id = tunnel::kJoinReply;
      answer.body.msgh_descriptor_count = 1;
      answer.port.name = bootstrap_;
      answer.port.disposition = MACH_MSG_TYPE_COPY_SEND;
      answer.port.type = MACH_MSG_PORT_DESCRIPTOR;
      if (mach_msg(&answer.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof answer, 0, MACH_PORT_NULL,
                   1000, MACH_PORT_NULL) != KERN_SUCCESS) {
        mach_msg_destroy(&answer.h);
      }
      return;
    }
    // Otherwise from the kernel, about one of the service ports.
    if (h->msgh_id == MACH_NOTIFY_PORT_DESTROYED && ports.size() == 1) {
      // The daemon that had checked in is gone, and the port is back.
      const auto it = by_port_.find(ports[0]);
      if (it == by_port_.end()) {
        mach_port_mod_refs(self, ports[0], MACH_PORT_RIGHT_RECEIVE, -1);
        return;
      }
      Service& sv = services_[it->second];
      sv.held = true;
      mach_port_t previous = MACH_PORT_NULL;
      mach_port_request_notification(self, sv.port, MACH_NOTIFY_PORT_DESTROYED, 0, notify_,
                                     MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
      if (sv.job >= 0 && jobs_[(size_t)sv.job].launches < kMaxLaunches) {
        watch(sv.port, true);  // the next message to it starts the daemon again
      } else {
        // Nobody will ever answer. Destroying the port tells everyone
        // waiting on it so, rather than leaving them to wait for good.
        if (sv.job >= 0) {
          log_warn("the daemon %s keeps failing; its service %s is withdrawn",
                   jobs_[(size_t)sv.job].label.c_str(), it->second.c_str());
        }
        mach_port_mod_refs(self, sv.port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(self, sv.port);
        sv.port = MACH_PORT_NULL;
        sv.held = false;
        sv.job = -1;
        by_port_.erase(it);
      }
    } else if (h->msgh_id == MACH_NOTIFY_DEAD_NAME && h->msgh_size >= 36) {
      // A port some program registered by hand died with that program.
      const mach_port_t dead = *(const mach_port_t*)(start + 32);
      const auto it = by_port_.find(dead);
      if (it != by_port_.end()) {
        Service& sv = services_[it->second];
        sv.port = MACH_PORT_NULL;
        sv.held = false;
        by_port_.erase(it);
      }
      mach_port_deallocate(self, dead);  // the notice itself holds one reference
      mach_port_deallocate(self, dead);
    } else {
      mach_msg_destroy(h);
    }
    return;
  }

  const u8* g = p + sizeof(tunnel::Meta);
  const bool be = meta->flags & tunnel::kBigEndian;
  const mach_port_t reply = h->msgh_remote_port;
  const s32 id = h->msgh_id;
  // The job the sender belongs to, if it is one of the daemons started here
  // (or something such a daemon started: they share its process group).
  const auto* trailer = (const mach_msg_audit_trailer_t*)(start + round_msg(h->msgh_size));
  const pid_t sender = (pid_t)trailer->msgh_audit.val[5];
  const int sender_job = job_of(sender);
  big_endian_ = be;  // daemons run on the same kind of CPU as their clients
  if (windows_first_) start_window_server();
  auto name_at = [&](u32 off) -> std::string {
    std::string s;
    for (u32 i = off; i < meta->size && i < off + 128 && g[i]; i++) s += (char)g[i];
    return s;
  };
  auto drop_ports = [&] {
    for (mach_port_t port : ports) {
      if (MACH_PORT_VALID(port)) mach_port_deallocate(self, port);
    }
  };

  if (getenv("COCAINE_SESSION_TRACE")) {  // for working on cocaine
    std::fprintf(stderr, "session: at %llu ms request %d \"%s\" from %s (process %d, group %d)\n",
                 (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000 % 1000000), id,
                 name_at(32).c_str(), sender_job >= 0 ? jobs_[sender_job].label.c_str() : "a program",
                 (int)sender, (int)getpgid(sender));
  }
  switch (id) {
    case kLookUp: {
      for (const auto& later : kPublishedLater) {
        if (name_at(32) != later.service) continue;
        start_window_server();
        bool have_window_server = false;
        for (const Job& job : jobs_) have_window_server |= job.label == later.job;
        if (!have_window_server && !told_no_windows_ && !big_endian_ && !getenv("COCAINE_DAEMONS")) {
          told_no_windows_ = true;
          log_warn("Intel programs can't open windows without the keys to Apple's Intel font server and "
                   "login window, which are stored encrypted (see the README). A universal program's "
                   "PowerPC half can (--arch ppc).");
        }
      }
      if (gate_ && sender_job < 0) {
        // Kept, rights and all, to be gone through again later.
        const size_t bytes = round_msg(h->msgh_size) + sizeof(mach_msg_audit_trailer_t);
        held_.emplace_back(start, start + bytes);
        return;
      }
      const std::string name = name_at(32);
      bool hidden = false;
      for (const auto& h : kHiddenServices) {
        hidden |= sender_job >= 0 && jobs_[(size_t)sender_job].label == h.job && name == h.service;
      }
      if (hidden) {
        reply_code(reply, id + 100, be, kUnknownService);
        break;
      }
      const auto it = services_.find(name);
      if (it == services_.end() || (!it->second.port && it->second.job < 0)) {
        // A name some daemon publishes for itself, once started?
        int publisher = -1;
        for (const auto& later : kPublishedLater) {
          if (name != later.service) continue;
          for (size_t i = 0; i < jobs_.size(); i++) {
            if (jobs_[i].label == later.job) publisher = (int)i;
          }
        }
        if (publisher >= 0 && publisher != sender_job &&
            jobs_[(size_t)publisher].launches < kMaxLaunches) {
          waiting_[name].push_back({reply, id + 100, be});
          launch(jobs_[(size_t)publisher]);
          // If it could not be started, nothing will publish the name, and
          // reap will never hear of it ending: say so now.
          if (jobs_[(size_t)publisher].pid <= 0) answer_waiting(name, MACH_PORT_NULL);
          break;
        }
        if (wanted_.insert(name).second) {
          log_info("a program looked for the service %s, which nothing here provides",
                   name.c_str());
        }
        reply_code(reply, id + 100, be, kUnknownService);
        break;
      }
      Service& sv = it->second;
      // A daemon asking for a service it is itself supposed to provide, and
      // hasn't collected yet, would be handed a port only it can answer and
      // then wait on itself. (The directory daemon does exactly this while
      // starting: a library it uses looks up a user, which normally means
      // asking the directory daemon.) Told there is no such service yet, the
      // library falls back on its own means.
      if (sender_job >= 0 && sender_job == sv.job && (sv.held || !sv.port)) {
        reply_code(reply, id + 100, be, kUnknownService);
        break;
      }
      if (name == "com.apple.pasteboard.1" && sv.job >= 0 && (!sv.port || sv.held)) {
        // Unlike most services, clients use pboard's shared memory as soon
        // as they have its port. It creates that memory before checking in.
        Job& job = jobs_[(size_t)sv.job];
        const bool first = !waiting_.count(name);
        waiting_[name].push_back({reply, id + 100, be});
        if (!sv.port && !make_port(sv, name)) {
          answer_waiting(name, MACH_PORT_NULL);
          break;
        }
        if (!job.pid) {
          launch(job);
          if (job.pid <= 0) {
            answer_waiting(name, MACH_PORT_NULL);
            break;
          }
        }
        if (first) {
          struct kevent64_s ev;
          EV_SET64(&ev, kPasteboardTimer, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 25000, 0, 0, 0);
          if (kevent64(kq_, &ev, 1, nullptr, 0, 0, nullptr) != 0) {
            log_warn("cannot wait for the pasteboard daemon: %s", strerror(errno));
            answer_waiting(name, MACH_PORT_NULL);
          }
        }
        break;
      }
      if (!sv.port && !make_port(sv, name)) {
        reply_code(reply, id + 100, be, kUnknownService);
        break;
      }
      reply_port(reply, id + 100, be, sv.port, MACH_MSG_TYPE_COPY_SEND);
      break;
    }
    case kCheckIn: {
      // A daemon collecting the port it is to serve. One nobody described in
      // advance is made on the spot.
      const std::string name = name_at(32);
      Service& sv = services_[name];
      if (!sv.port && !make_port(sv, name)) {
        reply_code(reply, id + 100, be, kUnknownService);
      } else if (!sv.held) {
        reply_code(reply, id + 100, be, kServiceActive);
      } else {
        watch(sv.port, false);
        sv.held = false;
        reply_port(reply, id + 100, be, sv.port, MACH_MSG_TYPE_MOVE_RECEIVE);
        answer_waiting(name, sv.port);
      }
      break;
    }
    case kRegister: {
      // A program publishing a port of its own under a name.
      const std::string name = name_at(48);
      if (ports.empty() || !MACH_PORT_VALID(ports[0]) || name.empty()) {
        drop_ports();
        reply_code(reply, id + 100, be, kUnknownService);
        break;
      }
      Service& sv = services_[name];
      if (sv.port) {
        if (sv.held) {
          drop_ports();
          reply_code(reply, id + 100, be, kServiceActive);
          break;
        }
        by_port_.erase(sv.port);
        mach_port_deallocate(self, sv.port);
      }
      sv.port = ports[0];
      sv.held = false;
      by_port_[sv.port] = name;
      answer_waiting(name, sv.port);
      if (name == "com.apple.windowserver.active") {
        // With the window server up, the login window follows, as at boot:
        // it is what sets a login session up.
        for (Job& job : jobs_) {
          if (job.label == "com.apple.loginwindow") {
            launch(job);
            login_pending_ = job.pid > 0;
          }
        }
        if (!login_pending_) open_gate();
      }
      mach_port_t previous = MACH_PORT_NULL;
      mach_port_request_notification(self, sv.port, MACH_NOTIFY_DEAD_NAME, 0, notify_,
                                     MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
      reply_code(reply, id + 100, be, KERN_SUCCESS);
      break;
    }
    case 409:  // subset: a private branch of the directory, for services that should
               // vanish with their owner. There is only the one directory here.
      drop_ports();
      reply_port(reply, id + 100, be, bootstrap_, MACH_MSG_TYPE_COPY_SEND);
      break;
    case kParent:  // the bootstrap port above this one: there is only this one
      reply_port(reply, id + 100, be, bootstrap_, MACH_MSG_TYPE_COPY_SEND);
      break;
    case kPostForkPing:  // a forked child introducing itself; nothing to do with that yet
      drop_ports();
      reply_code(reply, id + 100, be, KERN_SUCCESS);
      break;
    default:
      drop_ports();
      log_debug("launchd message %d is not handled", id);
      reply_code(reply, id + 100, be, kMigBadId);
      break;
  }
}

void Server::drain_requests() {
  static std::vector<u8> space(8192);
  for (;;) {
    auto* h = (mach_msg_header_t*)space.data();
    // With the kernel's note of who sent it, to tell a daemon's own requests
    // from its clients'.
    const kern_return_t kr =
        mach_msg(h,
                 MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT |
                     MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                     MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
                 0, (mach_msg_size_t)space.size(), main_set_, 0, MACH_PORT_NULL);
    if (kr == MACH_RCV_TOO_LARGE) {
      space.resize((size_t)h->msgh_size + 256);
      continue;
    }
    if (kr != KERN_SUCCESS) return;
    handle(h);
  }
}

// A message is waiting on a service port whose daemon isn't running. The
// message is not taken: it is the daemon's to read once it has checked in.
void Server::drain_watched() {
  // A message waiting for a daemon that has just died can be noticed before
  // the death is: the daemon would then look as if it were still running and
  // not be started again. So the dead are counted first. (reap also watches
  // their ports again, for when the death is noticed second.)
  reap();
  for (;;) {
    mach_msg_header_t h = {};
    const kern_return_t kr =
        mach_msg(&h, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY | MACH_RCV_TIMEOUT, 0,
                 sizeof h, watch_set_, 0, MACH_PORT_NULL);
    if (kr != MACH_RCV_TOO_LARGE) {
      if (kr == KERN_SUCCESS) mach_msg_destroy(&h);  // an empty message: nobody's
      return;
    }
    const mach_port_t port = h.msgh_local_port;
    watch(port, false);
    const auto it = by_port_.find(port);
    if (it == by_port_.end()) continue;
    const Service& sv = services_[it->second];
    if (sv.job >= 0) launch(jobs_[(size_t)sv.job]);
  }
}

// launchd's socket protocol (see liblaunch): a 16-byte header, then one
// packed object. Every object starts with three 64-bit big-endian fields: a
// type, then two that depend on it. Strings and containers are followed by
// their contents.
namespace launch {

enum : u64 { kDictionary = 1, kArray, kFd, kInteger, kReal, kBool, kString, kOpaque, kErrno, kMachPort };
constexpr u64 kMagic = 0xD2FEA02366B39A41ull;

void put64(std::string& o, u64 v) {
  for (int shift = 56; shift >= 0; shift -= 8) o += (char)(v >> shift);
}
u64 get64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}
std::string object(u64 type, u64 first, u64 second) {
  std::string o;
  put64(o, type);
  put64(o, first);
  put64(o, second);
  return o;
}
std::string string(const std::string& s) {
  std::string o = object(kString, 0, s.size()) + s;
  o.append(8 - s.size() % 8, '\0');  // the terminator, and padding to 8 bytes
  return o;
}
// 32-bit values sit in the first half of the field.
std::string boolean(bool b) { return object(kBool, (u64)b << 32, 0); }
std::string error(int e) { return object(kErrno, (u64)(u32)e << 32, 0); }
std::string integer(s64 v) { return object(kInteger, (u64)v, 0); }
// An array, or a dictionary given as alternating keys and values. After the
// header comes room for one pointer per item, which the reader fills in.
std::string list(u64 type, const std::vector<std::string>& items) {
  std::string o = object(type, 0, items.size());
  o.append(items.size() * 8, '\0');
  for (const std::string& i : items) o += i;
  return o;
}

}  // namespace launch

void Server::serve_socket(size_t index) {
  Job& job = jobs_[index];
  u8 buf[16384];
  const ssize_t n = recv(job.sock, buf, sizeof buf, MSG_DONTWAIT);
  if (n <= 0) {
    if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
      close(job.sock);  // taken off the queue's watch list by closing
      job.sock = -1;
    }
    return;
  }
  if (n < 16 + 24 || launch::get64(buf) != launch::kMagic) return;
  const u8* d = buf + 16;
  std::string answer = launch::error(EINVAL);
  if (launch::get64(d) == launch::kString) {
    const std::string request((const char*)d + 24, strnlen((const char*)d + 24, (size_t)n - 40));
    if (request == "CheckIn") {
      // The job's own description back to it. The ports are left blank: the
      // library fills each in by checking in over Mach, by name.
      std::vector<std::string> ports, args;
      for (const auto& sv : services_) {
        if (sv.second.job == (int)index) {
          ports.push_back(launch::string(sv.first));
          ports.push_back(launch::object(launch::kMachPort, 0, 0));
        }
      }
      for (const std::string& a : job.argv) args.push_back(launch::string(a));
      answer = launch::list(launch::kDictionary,
                            {launch::string("Label"), launch::string(job.label),
                             launch::string("OnDemand"), launch::boolean(true),
                             launch::string("TimeOut"), launch::integer(30),
                             launch::string("ProgramArguments"), launch::list(launch::kArray, args),
                             launch::string("MachServices"), launch::list(launch::kDictionary, ports)});
      log_debug("%s checked in over its socket", job.label.c_str());
    } else {
      log_debug("%s asked launchd \"%s\" over its socket, which is not handled",
                job.label.c_str(), request.c_str());
    }
  }
  std::string message;
  launch::put64(message, launch::kMagic);
  launch::put64(message, 16 + answer.size());
  message += answer;
  ::send(job.sock, message.data(), message.size(), 0);
}

// Where guests' log messages go. Programs and daemons report trouble through
// syslog, which is a datagram socket at a fixed place in the system root;
// with nobody listening the reports are lost, and they are often the only
// explanation a daemon gives for refusing something. They are written to the
// session's log.
void Server::open_syslog() {
  const std::string path = sysroot_ + "/private/var/run/syslog";
  struct sockaddr_un addr = {};
  if (path.size() >= sizeof addr.sun_path) return;
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, path.c_str(), sizeof addr.sun_path);
  syslog_ = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (syslog_ < 0) return;
  unlink(path.c_str());
  if (bind(syslog_, (const struct sockaddr*)&addr, sizeof addr) != 0) {
    close(syslog_);
    syslog_ = -1;
    return;
  }
  fcntl(syslog_, F_SETFD, FD_CLOEXEC);
  chmod(path.c_str(), 0666);
  struct kevent64_s ev;
  EV_SET64(&ev, (u64)syslog_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0, 0, 0);
  kevent64(kq_, &ev, 1, nullptr, 0, 0, nullptr);
}

void Server::read_syslog() {
  char buf[4096];
  ssize_t n;
  while ((n = recv(syslog_, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == 0)) buf[--n] = 0;
    std::fprintf(stderr, "syslog: %s\n", buf);
    if (login_pending_ && strstr(buf, "Login Window Application Started")) {
      // It is running, but hasn't set the session up yet: it says when it
      // has (kSessionReady). An application let in before that is told it
      // is the one in front and then quietly isn't, and never gets a key.
      // Should that word never come, don't hold applications for ever.
      struct kevent64_s ev;
      EV_SET64(&ev, kGateTimer, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 8000, 0, 0, 0);
      kevent64(kq_, &ev, 1, nullptr, 0, 0, nullptr);
    }
  }
}

void Server::reap() {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (Job& job : jobs_) {
      if (job.pid != pid) continue;
      job.pid = 0;
      // Whoever was waiting for it to publish something waits no longer.
      for (const auto& later : kPublishedLater) {
        if (job.label == later.job) answer_waiting(later.service, MACH_PORT_NULL);
      }
      if (job.label == "com.apple.pboard")
        answer_waiting("com.apple.pasteboard.1", MACH_PORT_NULL);
      if (job.label == "com.apple.WindowServer" || job.label == "com.apple.loginwindow") {
        login_pending_ = false;
        open_gate();
      }
      // Its ports are back here, or soon will be. A message already waiting
      // in one of them starts the daemon again, as the next to arrive would.
      const int index = (int)(&job - jobs_.data());
      if (job.launches < kMaxLaunches) {
        for (auto& entry : services_) {
          Service& sv = entry.second;
          if (sv.job == index && sv.held && sv.port) watch(sv.port, true);
        }
      }
      if (WIFEXITED(status)) {
        log_info("the daemon %s exited with status %d", job.label.c_str(), WEXITSTATUS(status));
      } else {
        log_info("the daemon %s was ended by signal %d", job.label.c_str(), WTERMSIG(status));
      }
    }
  }
}

void Server::shut_down() {
  // Every daemon this server ever started, with whatever it started itself:
  // asked to stop, given a moment, then stopped.
  std::vector<pid_t> groups = started_;
  for (pid_t group : groups) kill(-group, SIGTERM);
  for (int tries = 0; tries < 20; tries++) {
    reap();
    bool any = false;
    for (pid_t group : groups) any |= kill(-group, 0) == 0;
    if (!any) break;
    usleep(50000);
  }
  for (pid_t group : groups) kill(-group, SIGKILL);
  reap();
  struct stat st;
  if (stat(log_path_.c_str(), &st) == 0 && st.st_size == 0) unlink(log_path_.c_str());
  // The session's shared memory objects, which live as files in its folder.
  const std::string dir = tmp_ + name_, shm = dir + "/shm";
  if (DIR* d = opendir(shm.c_str())) {
    while (const dirent* e = readdir(d)) {
      if (e->d_type == DT_REG) unlink((shm + "/" + e->d_name).c_str());
    }
    closedir(d);
  }
  rmdir(shm.c_str());
  unlink((dir + "/gl-pacing").c_str());
  rmdir(dir.c_str());
  if (syslog_ >= 0) unlink((sysroot_ + "/private/var/run/syslog").c_str());
}

int Server::run() {
  setsid();  // no terminal: the keyboard's interrupt is for the session's programs
  signal(SIGTERM, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  // This process was forked from a guest at some arbitrary moment and holds
  // copies of whatever that guest had open. None of it is ours to keep: a
  // pipe held open here would never reach end-of-file for its reader. What
  // the server and its daemons print goes to the session's log instead.
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) strlcpy(tmp, "/tmp/", sizeof tmp);
  tmp_ = tmp;
  log_path_ = tmp_ + "cocaine-session-" + std::to_string((int)root_) + ".log";
  // The logs of finished sessions are kept for looking into afterwards, but
  // only the five newest.
  if (DIR* d = opendir(tmp_.c_str())) {
    std::vector<std::pair<time_t, std::string>> old;
    while (const dirent* e = readdir(d)) {
      int pid = 0;
      if (sscanf(e->d_name, "cocaine-session-%d.log", &pid) != 1 || pid == (int)root_) continue;
      if (kill(pid, 0) == 0 || errno != ESRCH) continue;  // still running
      struct stat st;
      const std::string p = tmp_ + e->d_name;
      if (stat(p.c_str(), &st) == 0) old.push_back({st.st_mtime, p});
    }
    closedir(d);
    std::sort(old.begin(), old.end());
    for (size_t k = 0; k + 5 < old.size(); k++) unlink(old[k].second.c_str());
  }
  const int limit = getdtablesize();
  for (int fd = 0; fd < limit; fd++) close(fd);
  open("/dev/null", O_RDONLY);                                      // 0
  open("/dev/null", O_WRONLY);                                      // 1
  open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);     // 2

  if (!take_bootstrap_port()) {
    log_warn("the session server could not take over the session's ports");
    return 1;
  }

  const mach_port_t self = mach_task_self();
  mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &notify_);
  mach_port_allocate(self, MACH_PORT_RIGHT_PORT_SET, &main_set_);
  mach_port_allocate(self, MACH_PORT_RIGHT_PORT_SET, &watch_set_);
  mach_port_insert_member(self, bootstrap_, main_set_);
  mach_port_insert_member(self, named_, main_set_);
  mach_port_insert_member(self, notify_, main_set_);

  add_current_user();
  configure_window_server();
  load_jobs("/System/Library/LaunchDaemons");
  load_jobs("/System/Library/LaunchAgents");
  log_info("session server for process %d: %zu daemons on call", (int)root_, jobs_.size());

  const int kq = kq_ = kqueue();
  struct kevent64_s ev;
  EV_SET64(&ev, (u64)root_, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, 0, 0, 0);
  if (kevent64(kq, &ev, 1, nullptr, 0, 0, nullptr) != 0) {
    shut_down();  // the root has already gone
    return 0;
  }
  EV_SET64(&ev, main_set_, EVFILT_MACHPORT, EV_ADD | EV_ENABLE, 0, 0, 0, 0, 0);
  kevent64(kq, &ev, 1, nullptr, 0, 0, nullptr);
  EV_SET64(&ev, watch_set_, EVFILT_MACHPORT, EV_ADD | EV_ENABLE, 0, 0, 0, 0, 0);
  kevent64(kq, &ev, 1, nullptr, 0, 0, nullptr);
  open_syslog();
  for (int sig : {SIGCHLD, SIGTERM}) {
    EV_SET64(&ev, (u64)sig, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, 0, 0, 0);
    kevent64(kq, &ev, 1, nullptr, 0, 0, nullptr);
  }

  for (;;) {
    struct kevent64_s got[8];
    const int n = kevent64(kq, nullptr, 0, got, 8, 0, nullptr);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    bool done = false;
    for (int i = 0; i < n; i++) {
      if (got[i].filter == EVFILT_PROC) {
        done = true;
      } else if (got[i].filter == EVFILT_SIGNAL) {
        if (got[i].ident == SIGTERM) done = true;
        reap();
      } else if (got[i].filter == EVFILT_TIMER) {
        if (got[i].ident == kPasteboardTimer) {
          if (waiting_.count("com.apple.pasteboard.1")) {
            log_warn("the pasteboard daemon was not ready in time");
            answer_waiting("com.apple.pasteboard.1", MACH_PORT_NULL);
          }
        } else if (got[i].ident == kGateTimer && gate_) {
          log_warn("the window server's session was not ready in time; letting programs in anyway");
          login_pending_ = false;
          open_gate();
        }
      } else if (got[i].filter == EVFILT_READ && (int)got[i].ident == syslog_) {
        read_syslog();
      } else if (got[i].filter == EVFILT_READ) {
        if (got[i].udata >= 1 && got[i].udata <= jobs_.size()) serve_socket((size_t)got[i].udata - 1);
      } else if (got[i].ident == main_set_) {
        drain_requests();
      } else if (got[i].ident == watch_set_) {
        drain_watched();
      }
    }
    if (done) break;
  }
  shut_down();
  return 0;
}

}  // namespace

int run_session_server(const std::string& self_path, const std::string& sysroot, int root_pid,
                       const std::string& name, bool powerpc, bool windows) {
  Server server(self_path, sysroot, (pid_t)root_pid, name, powerpc, windows);
  return server.run();
}
