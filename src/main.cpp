// cocaine: run old Mac programs (PowerPC and 32-bit Intel) on Apple Silicon.

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <optional>
#include <vector>

#include "common.h"
#include "kernel/process.h"
#include "macho/macho.h"
#include "mem/guest_memory.h"
#include "session/session.h"
#include "session/ui_scale.h"
#include "viewer/picker.h"
#include "viewer/viewer.h"

extern char** environ;

namespace {

const char kUsage[] =
    "usage: cocaine [options] <program | Some.app> [arguments for the program]\n"
    "\n"
    "Runs a PowerPC or 32-bit Intel Mac OS X program.\n"
    "\n"
    "options:\n"
    "  --arch ppc|i386   which half of a universal program to run\n"
    "  --sysroot DIR     Mac OS X system root holding dyld and the system libraries\n"
    "                    (default: $COCAINE_SYSROOT)\n"
    "  --env NAME=VALUE  set an environment variable for the program (repeatable)\n"
    "  --info            describe the program and exit without running it\n"
    "  --strace          log every system call the program makes\n"
    "  --trace           log every instruction the program runs (very slow)\n"
    "  --trace-registers ... along with the registers before each one\n"
    "  --stats           report how many instructions ran, and how fast\n"
    "  --max-insns N     stop after N instructions\n"
    "  -v, -vv           more logging\n"
    "  --version, --help\n";

bool is_dir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Finds the program inside an application bundle, using the bundle's
// Info.plist and falling back to the bundle's own name.
std::string bundle_executable(const std::string& dir, std::string* bundle_id = nullptr) {
  std::string name;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)dir.c_str(),
                                                         (CFIndex)dir.size(), true);
  if (url) {
    if (CFDictionaryRef info = CFBundleCopyInfoDictionaryInDirectory(url)) {
      if (bundle_id) {
        const void* identifier = CFDictionaryGetValue(info, CFSTR("CFBundleIdentifier"));
        char id[PATH_MAX];
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() &&
            CFStringGetCString((CFStringRef)identifier, id, sizeof id, kCFStringEncodingUTF8)) {
          *bundle_id = id;
        }
      }
      const void* v = CFDictionaryGetValue(info, CFSTR("CFBundleExecutable"));
      char buf[PATH_MAX];
      if (v && CFGetTypeID(v) == CFStringGetTypeID() &&
          CFStringGetFileSystemRepresentation((CFStringRef)v, buf, sizeof buf)) {
        name = buf;
      }
      CFRelease(info);
    }
    CFRelease(url);
  }
  if (name.empty()) {
    std::string base = dir;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const size_t slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.rfind('.');
    name = dot == std::string::npos ? base : base.substr(0, dot);
  }
  return dir + "/Contents/MacOS/" + name;
}

void print_info(const MachImage& img) {
  static const char* const kTypes[] = {"?",     "object", "program",        "fvmlib", "core",
                                       "preload", "library", "dynamic linker", "bundle"};
  std::printf("%s\n", img.path.c_str());
  std::printf("  architecture: %s (%s-endian)\n", arch_name(img.arch),
              img.big_endian ? "big" : "little");
  std::printf("  kind:         %s\n", img.filetype < 9 ? kTypes[img.filetype] : "?");
  if (img.has_entry) std::printf("  entry point:  0x%08x\n", img.entry);
  if (img.has_main) std::printf("  entry point:  found by dyld (LC_MAIN)\n");
  std::printf("  linking:      %s\n",
              img.dylinker.empty() ? "static" : ("dynamic, via " + img.dylinker).c_str());
  std::printf("  segments:\n");
  for (const MachSegment& s : img.segments) {
    std::printf("    %-16s 0x%08x-0x%08x  %c%c%c\n", s.name.c_str(), s.vmaddr, s.vmaddr + s.vmsize,
                (s.initprot & 1) ? 'r' : '-', (s.initprot & 2) ? 'w' : '-',
                (s.initprot & 4) ? 'x' : '-');
  }
  if (!img.dylibs.empty()) {
    std::printf("  libraries:\n");
    for (const std::string& d : img.dylibs) std::printf("    %s\n", d.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::optional<Arch> want;
  bool info_only = false;
  std::vector<std::string> extra_env;
  std::optional<Arch> prefer;
  int session_root = 0;  // not zero: run as the session server for that process
  bool session_windows = false;
  bool pick = false;  // run as the file picker's helper
  if (const char* root = getenv("COCAINE_SYSROOT")) opt.sysroot = root;

  int i = 1;
  for (; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--") {
      i++;
      break;
    }
    if (a.empty() || a[0] != '-') break;
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "cocaine: %s needs a value\n", a.c_str());
        exit(125);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (a == "--version") {
      std::puts("cocaine 0.1");
      return 0;
    } else if (a == "--arch") {
      const std::string v = value();
      if (v == "ppc") {
        want = Arch::PPC;
      } else if (v == "i386") {
        want = Arch::I386;
      } else {
        std::fprintf(stderr, "cocaine: --arch takes ppc or i386, not '%s'\n", v.c_str());
        return 125;
      }
    } else if (a == "--prefer-arch") {
      // Used when one guest program runs another: stay on the same CPU if
      // the new program has that half, otherwise take what it has.
      prefer = std::string(value()) == "ppc" ? Arch::PPC : Arch::I386;
    } else if (a == "--argv0") {
      opt.argv0 = value();
    } else if (a == "--as-root") {
      // Used for system daemons, which check that they are running as root.
      opt.as_root = true;
    } else if (a == "--session") {
      // Used when a guest of a session starts another program: the session to join.
      opt.session = value();
    } else if (a == "--view") {
      // Used by the process running the guest's window server: show its display.
      return run_viewer(value());
    } else if (a == "--pick") {
      // Used when a guest shows an open or save panel: answer its request
      // (on descriptor 3) with this Mac's file picker. --session follows.
      pick = true;
    } else if (a == "--session-server") {
      // Used by a session's first process to start the stand-in for launchd.
      session_root = atoi(value());
    } else if (a == "--windows") {
      // With --session-server: the session's first program is one with windows.
      session_windows = true;
    } else if (a == "--sysroot") {
      opt.sysroot = value();
    } else if (a == "--env") {
      extra_env.push_back(value());
    } else if (a == "--info") {
      info_only = true;
    } else if (a == "--strace") {
      opt.strace = true;
    } else if (a == "--trace") {
      opt.trace = true;
    } else if (a == "--trace-registers") {
      opt.trace = opt.trace_registers = true;
    } else if (a == "--stats") {
      opt.stats = true;
    } else if (a == "--max-insns") {
      opt.max_insns = strtoull(value(), nullptr, 0);
    } else if (a == "-v") {
      g_verbosity = 1;
    } else if (a == "-vv") {
      g_verbosity = 2;
    } else {
      std::fprintf(stderr, "cocaine: unknown option %s\n\n%s", a.c_str(), kUsage);
      return 125;
    }
  }
  if (pick) return run_picker(opt.session);
  if (i >= argc && !session_root) {
    std::fputs(kUsage, stderr);
    return 125;
  }
  if (!opt.sysroot.empty()) {
    char root[PATH_MAX];
    if (!realpath(opt.sysroot.c_str(), root)) {
      std::fprintf(stderr, "cocaine: system root %s: %s\n", opt.sysroot.c_str(), strerror(errno));
      return 127;
    }
    opt.sysroot = root;
  }
  char self[PATH_MAX];
  uint32_t self_size = sizeof self;
  if (_NSGetExecutablePath(self, &self_size) == 0) opt.self_path = self;
  if (session_root) {
    return run_session_server(opt.self_path, opt.sysroot, session_root, opt.session,
                              prefer && *prefer == Arch::PPC, session_windows);
  }

  std::string target = argv[i];
  std::string bundle_id;
  if (is_dir(target)) target = bundle_executable(target, &bundle_id);
  char resolved[PATH_MAX];
  if (!realpath(target.c_str(), resolved)) {
    std::fprintf(stderr, "cocaine: %s: %s\n", target.c_str(), strerror(errno));
    return 127;
  }
  if (bundle_id.empty()) {
    const std::string path = resolved;
    const size_t app = path.rfind(".app/");
    if (app != std::string::npos) bundle_executable(path.substr(0, app + 4), &bundle_id);
  }

  std::string err;
  MachImage exe;
  u8 header[40];
  size_t header_size = 0;
  if (FILE* f = std::fopen(resolved, "rb")) {
    header_size = std::fread(header, 1, sizeof header, f);
    std::fclose(f);
  }
  const bool pef = is_powerpc_pef(header, header_size);
  std::string launcher;
  if (pef) {
    if (want && *want == Arch::I386) {
      std::fprintf(stderr, "cocaine: %s: is a PowerPC PEF/CFM file, not i386\n", resolved);
      return 126;
    }
    if (info_only) {
      std::printf("%s\n  architecture: ppc (big-endian)\n  kind:         PowerPC PEF/CFM\n", resolved);
      return 0;
    }
    if (opt.sysroot.empty()) {
      std::fprintf(stderr, "cocaine: PowerPC PEF/CFM programs need a Leopard system root\n");
      return 126;
    }
    launcher = opt.sysroot + "/System/Library/Frameworks/Carbon.framework/Versions/A/Support/LaunchCFMApp";
    if (!load_macho(launcher, Arch::PPC, exe, err)) {
      std::fprintf(stderr, "cocaine: cannot load Leopard's LaunchCFMApp: %s\n", err.c_str());
      return 126;
    }
  } else if (prefer && !want && load_macho(resolved, prefer, exe, err)) {
    // Loaded the preferred half.
  } else if (!load_macho(resolved, want, exe, err)) {
    std::fprintf(stderr, "cocaine: %s\n", err.c_str());
    return 126;
  }
  if (info_only) {
    print_info(exe);
    return 0;
  }
  if (!configure_legacy_ui_scale(exe.dylibs, err, bundle_id) || !configure_host_display(err)) {
    std::fprintf(stderr, "cocaine: %s\n", err.c_str());
    return 126;
  }

  GuestMemory mem;
  if (!mem.init(exe.big_endian, err)) {
    std::fprintf(stderr, "cocaine: %s\n", err.c_str());
    return 126;
  }

  std::vector<std::string> guest_argv;
  if (pef) {
    std::string guest_target = resolved;
    if (guest_target.size() > opt.sysroot.size() &&
        guest_target.compare(0, opt.sysroot.size(), opt.sysroot) == 0 &&
        guest_target[opt.sysroot.size()] == '/') guest_target.erase(0, opt.sysroot.size());
    guest_argv = {launcher, guest_target};
  } else {
    guest_argv = {resolved};
  }
  for (int k = i + 1; k < argc; k++) guest_argv.push_back(argv[k]);
  // The guest inherits this environment, minus the DYLD_ variables: those
  // steer this Mac's dynamic linker and name libraries the guest can't load.
  // Variables meant for the guest's own dyld come in through --env.
  std::vector<std::string> guest_env;
  for (char** e = environ; *e; e++) {
    if (strncmp(*e, "DYLD_", 5) != 0) guest_env.push_back(*e);
  }
  guest_env.insert(guest_env.end(), extra_env.begin(), extra_env.end());

  Process proc(exe.arch, mem, opt);
  if (!proc.load(exe, guest_argv, guest_env, err)) {
    std::fprintf(stderr, "cocaine: %s\n", err.c_str());
    return 126;
  }
  const int status = proc.run();
  // Leave without unwinding: other guest threads may still be running, and
  // must not find the process's memory and locks torn down under them.
  fflush(nullptr);
  _exit(status);
}
