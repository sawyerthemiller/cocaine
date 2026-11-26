// sysctl: how programs ask what machine and system they are running on.
//
// The answers describe the Mac the guest believes it is on: a two-CPU
// Power Mac G4 or first-generation Intel MacBook running Mac OS X 10.5.

#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include "kernel/process.h"

namespace {

struct Entry {
  const char* name;
  int mib0, mib1;  // the classic numeric name, or -1 if it only has a text name
  enum Type { Int, Quad, Text } type;
  u64 value;
  std::string text;
};

constexpr u64 kMemory = 0x80000000ull;  // 2 GB

std::vector<Entry> build_table(Arch arch) {
  const bool ppc = arch == Arch::PPC;
  char host[256] = "localhost";
  gethostname(host, sizeof host);
  const auto I = Entry::Int;
  const auto Q = Entry::Quad;
  const auto T = Entry::Text;
  return {
      // kern.*
      {"kern.ostype", 1, 1, T, 0, "Darwin"},
      {"kern.osrelease", 1, 2, T, 0, "9.0.0"},
      {"kern.osrevision", 1, 3, I, 199506, ""},
      {"kern.version", 1, 4, T, 0,
       std::string("Darwin Kernel Version 9.0.0: Tue Oct  9 21:35:55 PDT 2007; "
                   "root:xnu-1228~1/RELEASE_") + (ppc ? "PPC" : "I386")},
      {"kern.maxproc", 1, 6, I, 532, ""},
      {"kern.argmax", 1, 8, I, 262144, ""},
      {"kern.securelevel", 1, 9, I, 0, ""},
      {"kern.hostname", 1, 10, T, 0, host},
      {"kern.ngroups", 1, 18, I, 16, ""},
      {"kern.maxfilesperproc", 1, 29, I, 10240, ""},
      {"kern.usrstack", 1, 35, I, 0xC0000000u, ""},
      // The program the kernel hands other-CPU binaries to: Rosetta, on Intel.
      // It has to be there. The Intel Objective-C runtime asks for it while
      // loading every library and, given no answer, skips that library's
      // categories, which breaks CoreFoundation.
      {"kern.exec_archhandler", 1, 45, T, 0,
       ppc ? "/System/Library/CoreServices/Classic Startup.app/Contents/Resources/TruBlueEnvironment"
           : "/usr/libexec/oah/translate"},
      {"kern.procname", 1, 62, T, 0, ""},
      {"kern.osversion", 1, 65, T, 0, "9A581"},
      {"kern.safeboot", 1, 66, I, 0, ""},
      // hw.*
      {"hw.machine", 6, 1, T, 0, ppc ? "Power Macintosh" : "i386"},
      {"hw.model", 6, 2, T, 0, ppc ? "PowerMac3,6" : "MacBook1,1"},
      {"hw.ncpu", 6, 3, I, 2, ""},
      {"hw.byteorder", 6, 4, I, ppc ? 4321u : 1234u, ""},
      {"hw.physmem", 6, 5, I, kMemory, ""},
      {"hw.usermem", 6, 6, I, kMemory - (256u << 20), ""},
      {"hw.pagesize", 6, 7, I, 4096, ""},
      {"hw.machine_arch", 6, 12, T, 0, ppc ? "ppc" : "i386"},
      {"hw.vectorunit", 6, 13, I, 1, ""},
      // Named frequency queries return 64-bit values; the older numeric
      // interfaces return 32-bit values.
      {"hw.busfrequency", -1, 0, Q, ppc ? 133000000u : 664000000u, ""},
      {"hw.cpufrequency", -1, 0, Q, ppc ? 1250000000u : 2000000000u, ""},
      {nullptr, 6, 14, I, ppc ? 133000000u : 664000000u, ""},
      {nullptr, 6, 15, I, ppc ? 1250000000u : 2000000000u, ""},
      {"hw.cachelinesize", 6, 16, I, ppc ? 32u : 64u, ""},
      {"hw.l1icachesize", 6, 17, I, 32768, ""},
      {"hw.l1dcachesize", 6, 18, I, 32768, ""},
      {"hw.l2settings", 6, 19, I, 1, ""},
      {"hw.l2cachesize", 6, 20, I, ppc ? 262144u : 2097152u, ""},
      {"hw.l3cachesize", 6, 22, I, 0u, ""},
      {"hw.tbfrequency", -1, 0, Q, 1000000000u, ""},  // the time base ticks in nanoseconds here
      {nullptr, 6, 23, I, 1000000000u, ""},
      {"hw.memsize", 6, 24, Q, kMemory, ""},
      {"hw.availcpu", 6, 25, I, 2, ""},
      // Names with no fixed number.
      {"vm.shared_region_version", -1, 0, I, 3, ""},
      {"hw.activecpu", -1, 0, I, 2, ""},
      {"hw.physicalcpu", -1, 0, I, 2, ""},
      {"hw.physicalcpu_max", -1, 0, I, 2, ""},
      {"hw.logicalcpu", -1, 0, I, 2, ""},
      {"hw.logicalcpu_max", -1, 0, I, 2, ""},
      {"hw.cputype", -1, 0, I, ppc ? 18u : 7u, ""},
      {"hw.cpusubtype", -1, 0, I, ppc ? 10u : 4u, ""},
      {"hw.cpufamily", -1, 0, I, ppc ? 0x77C184AEu : 0x73D67300u, ""},  // G4, or Core Duo
      {"hw.cpu64bit_capable", -1, 0, I, 0, ""},
      {"hw.busfrequency_min", -1, 0, Q, ppc ? 133000000u : 664000000u, ""},
      {"hw.busfrequency_max", -1, 0, Q, ppc ? 133000000u : 664000000u, ""},
      {"hw.cpufrequency_min", -1, 0, Q, ppc ? 1250000000u : 2000000000u, ""},
      {"hw.cpufrequency_max", -1, 0, Q, ppc ? 1250000000u : 2000000000u, ""},
      {"hw.optional.floatingpoint", -1, 0, I, 1, ""},
      {"hw.optional.altivec", -1, 0, I, ppc ? 1u : 0u, ""},
      {"hw.optional.graphicsops", -1, 0, I, ppc ? 1u : 0u, ""},
      {"hw.optional.stfiwx", -1, 0, I, ppc ? 1u : 0u, ""},
      {"hw.optional.fsqrt", -1, 0, I, 0, ""},
      {"hw.optional.dcba", -1, 0, I, 0, ""},
      {"hw.optional.datastreams", -1, 0, I, ppc ? 1u : 0u, ""},
      {"hw.optional.dcbtstreams", -1, 0, I, 0, ""},
      {"hw.optional.64bitops", -1, 0, I, 0, ""},
      {"hw.optional.mmx", -1, 0, I, ppc ? 0u : 1u, ""},
      {"hw.optional.sse", -1, 0, I, ppc ? 0u : 1u, ""},
      {"hw.optional.sse2", -1, 0, I, ppc ? 0u : 1u, ""},
      {"hw.optional.sse3", -1, 0, I, ppc ? 0u : 1u, ""},
      {"hw.optional.supplementalsse3", -1, 0, I, 0, ""},
      {"hw.optional.x86_64", -1, 0, I, 0, ""},
  };
}

// Text-only names get made-up numbers in a range the real kernel doesn't use.
constexpr int kNamedBase = 0x4000;

}  // namespace

SysRet Process::sys_sysctl(u32 name, u32 namelen, u32 oldp, u32 oldlenp, u32 newp, u32 newlen) {
  if (namelen < 1 || namelen > 12) return SysRet::err(EINVAL);
  if (!mem_.range_ok(name, namelen * 4)) return SysRet::err(EFAULT);
  int mib[12] = {};
  for (u32 i = 0; i < namelen; i++) mib[i] = (int)mem_.r32(name + 4 * i);

  static const std::vector<Entry> table = build_table(arch_);

  // Copies an answer out the way sysctl does: the length always comes back,
  // the data only if a buffer was given and is big enough.
  auto answer = [&](const void* data, u32 size) -> SysRet {
    if (!oldlenp) return SysRet::ok();
    if (!mem_.range_ok(oldlenp, 4, GuestMemory::kRead | GuestMemory::kWrite)) return SysRet::err(EFAULT);
    const u32 room = mem_.r32(oldlenp);
    mem_.w32(oldlenp, size);
    if (!oldp) return SysRet::ok();
    if (!mem_.range_ok(oldp, room < size ? room : size, GuestMemory::kWrite)) return SysRet::err(EFAULT);
    memcpy(mem_.host(oldp), data, room < size ? room : size);
    return room < size ? SysRet::err(ENOMEM) : SysRet::ok();
  };
  auto answer_entry = [&](const Entry& e) -> SysRet {
    if (e.type == Entry::Text) return answer(e.text.c_str(), (u32)e.text.size() + 1);
    if (e.type == Entry::Quad) {
      u64 v = e.value;
      if (mem_.big_endian()) v = __builtin_bswap64(v);
      return answer(&v, 8);
    }
    u32 v = (u32)e.value;
    if (mem_.big_endian()) v = __builtin_bswap32(v);
    return answer(&v, 4);
  };

  // The current process name is writable, and short output buffers receive
  // a terminated, truncated name rather than ENOMEM.
  if (namelen == 2 && mib[0] == 1 && mib[1] == 62) {
    std::lock_guard<std::mutex> lk(proc_name_mu_);
    std::string old = proc_name_;
    if (oldp && oldlenp) {
      if (!mem_.range_ok(oldlenp, 4, GuestMemory::kRead)) return SysRet::err(EFAULT);
      const u32 room = mem_.r32(oldlenp);
      if (room && room <= old.size()) old.resize(room - 1);
    }
    const SysRet result = answer(old.c_str(), (u32)old.size() + 1);
    if (result.kind == SysRet::Error || !newp) return result;
    if (newlen > 32) return SysRet::err(EINVAL);
    if (!mem_.range_ok(newp, newlen, GuestMemory::kRead)) return SysRet::err(EFAULT);
    const char* value = (const char*)mem_.host(newp);
    proc_name_.assign(value, strnlen(value, newlen));
    return SysRet::ok();
  }

  // {0, 3}: translate a text name into numbers.
  if (namelen == 2 && mib[0] == 0 && mib[1] == 3) {
    std::string text;
    if (!mem_.range_ok(newp, newlen)) return SysRet::err(EFAULT);
    text.assign((const char*)mem_.host(newp), newlen);
    while (!text.empty() && text.back() == 0) text.pop_back();
    for (size_t i = 0; i < table.size(); i++) {
      if (!table[i].name || text != table[i].name) continue;
      u32 out[2];
      out[0] = (u32)(table[i].mib0 < 0 ? kNamedBase : table[i].mib0);
      out[1] = (u32)(table[i].mib0 < 0 ? (int)i : table[i].mib1);
      if (mem_.big_endian()) {
        out[0] = __builtin_bswap32(out[0]);
        out[1] = __builtin_bswap32(out[1]);
      }
      return answer(out, 8);
    }
    log_info("sysctl: unknown name %s", text.c_str());
    return SysRet::err(ENOENT);
  }

  // kern.proc.pid (pid): the kernel's record of one process. Daemons ask
  // about their clients this way (what is it called, whose is it).
  if (namelen == 4 && mib[0] == 1 && mib[1] == 14 && mib[2] == 1) {
    GuestProcess gp;
    if (!describe_process(mib[3], gp)) {
      if (oldlenp && mem_.range_ok(oldlenp, 4)) mem_.w32(oldlenp, 0);  // no such process: an empty answer
      return SysRet::ok();
    }
    // 492 bytes: the process part (196), then the "extra" part with the
    // credentials and relatives. Only the fields programs read are filled.
    u8 rec[492] = {};
    const bool be = mem_.big_endian();
    auto put32 = [&](u32 off, u32 v) {
      if (be) v = __builtin_bswap32(v);
      memcpy(rec + off, &v, 4);
    };
    put32(0, (u32)gp.started);     // start time, seconds
    put32(16, 0x4004);             // flags: has run a program, has a controlling terminal
    rec[20] = 2;                   // state: runnable
    put32(24, (u32)gp.pid);
    put32(28, (u32)gp.ppid);
    strlcpy((char*)rec + 163, gp.name.c_str(), 17);
    const u32 e = 196;
    put32(e + 84, gp.uid);         // real and saved user
    put32(e + 88, gp.uid);
    put32(e + 92, gp.gid);         // real and saved group
    put32(e + 96, gp.gid);
    put32(e + 104, 1);             // credentials: one reference,
    put32(e + 108, gp.uid);        // the effective user,
    rec[e + 112 + (be ? 1 : 0)] = 1;  // one group,
    put32(e + 116, gp.gid);        // which is this
    put32(e + 220, (u32)gp.ppid);
    put32(e + 224, (u32)gp.pgid);
    put32(e + 232, 0xFFFFFFFFu);   // no terminal device
    return answer(rec, sizeof rec);
  }

  // kern.procargs2 (pid): argc, the program's path, then its arguments and
  // environment as a run of strings.
  if (namelen == 3 && mib[0] == 1 && mib[1] == 49) {
    if (mib[2] != getpid()) return SysRet::err(EINVAL);
    std::vector<u8> out(4);
    u32 argc = (u32)argv_.size();
    if (mem_.big_endian()) argc = __builtin_bswap32(argc);
    memcpy(out.data(), &argc, 4);
    out.insert(out.end(), exe_path_.begin(), exe_path_.end());
    out.push_back(0);
    while (out.size() % 4) out.push_back(0);
    for (const std::string& s : argv_) {
      out.insert(out.end(), s.begin(), s.end());
      out.push_back(0);
    }
    for (const std::string& s : envp_) {
      out.insert(out.end(), s.begin(), s.end());
      out.push_back(0);
    }
    return answer(out.data(), (u32)out.size());
  }

  // kern.affinity and kern.translate (pid): "is this process running under
  // Classic or Rosetta?" As far as the guest can tell, no.
  if (namelen >= 2 && mib[0] == 1 && (mib[1] == 43 || mib[1] == 44)) {
    const u32 no = 0;
    return answer(&no, 4);
  }

  // kern.boottime: when this pretend Mac started.
  if (namelen == 2 && mib[0] == 1 && mib[1] == 21) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    u32 tv[2] = {(u32)now.tv_sec - 3600, 0};
    if (mem_.big_endian()) tv[0] = __builtin_bswap32(tv[0]);
    return answer(tv, 8);
  }

  if (namelen == 2) {
    if (mib[0] == kNamedBase && mib[1] >= 0 && (size_t)mib[1] < table.size()) {
      return answer_entry(table[(size_t)mib[1]]);
    }
    for (const Entry& e : table) {
      if (e.mib0 == mib[0] && e.mib1 == mib[1]) return answer_entry(e);
    }
  }

  std::string spelled;
  for (u32 i = 0; i < namelen; i++) spelled += (i ? "." : "") + std::to_string(mib[i]);
  log_warn("unimplemented sysctl {%s}", spelled.c_str());
  return SysRet::err(ENOENT);
}


// Every guest process is, to this Mac, a process called "cocaine". What it is
// to the guest (which program, and whether it was told it is root) is read
// from how that cocaine was started.
bool Process::describe_process(int pid, GuestProcess& out) const {
  struct kinfo_proc kp;
  size_t len = sizeof kp;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) return false;
  out.pid = pid;
  out.ppid = kp.kp_eproc.e_ppid;
  out.pgid = kp.kp_eproc.e_pgid;
  out.uid = kp.kp_eproc.e_ucred.cr_uid;
  out.gid = kp.kp_eproc.e_pcred.p_rgid;
  out.started = kp.kp_proc.p_starttime.tv_sec;
  out.name = kp.kp_proc.p_comm;
  if (pid == getpid()) {
    if (opt_.as_root) out.uid = out.gid = 0;
    std::lock_guard<std::mutex> lk(proc_name_mu_);
    out.name = proc_name_;
    return true;
  }

  // Another process: its command line, as the host kernel keeps it.
  int amib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  std::vector<char> buf(65536);
  len = buf.size();
  if (sysctl(amib, 3, buf.data(), &len, nullptr, 0) != 0 || len < 8) return true;
  int argc;
  memcpy(&argc, buf.data(), 4);
  const char* p = buf.data() + 4;
  const char* const end = buf.data() + len;
  p += strnlen(p, (size_t)(end - p));  // the executable's path
  while (p < end && !*p) p++;
  std::vector<std::string> args;
  for (int i = 0; i < argc && p < end; i++) {
    args.emplace_back(p, strnlen(p, (size_t)(end - p)));
    p += args.back().size() + 1;
  }
  if (args.empty() || args[0].size() < 7 || args[0].compare(args[0].size() - 7, 7, "cocaine") != 0) {
    return true;  // not a guest: an ordinary process of this Mac
  }
  std::string program;
  static const char* const kTakesValue[] = {"--arch", "--prefer-arch", "--argv0", "--sysroot",
                                            "--env", "--max-insns", "--session"};
  for (size_t i = 1; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a == "--as-root") out.uid = out.gid = 0;
    if (a == "--") {
      if (i + 1 < args.size()) program = args[i + 1];
      break;
    }
    if (a.empty() || a[0] != '-') {
      program = a;
      break;
    }
    for (const char* opt : kTakesValue) {
      if (a == opt) i++;
    }
  }
  while (!program.empty() && program.back() == '/') program.pop_back();
  const size_t slash = program.rfind('/');
  if (slash != std::string::npos) program = program.substr(slash + 1);
  if (program.size() > 4 && program.compare(program.size() - 4, 4, ".app") == 0) {
    program.erase(program.size() - 4);
  }
  if (!program.empty()) out.name = program;
  return true;
}
