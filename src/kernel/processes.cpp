// More than one guest process.
//
// A guest process is a host process running cocaine, so the host's own
// process machinery does the heavy lifting:
//
//   fork   forks cocaine. The child inherits a copy of everything, guest
//          memory and open files included, which is exactly what fork means.
//   exec   re-runs cocaine on the new program, in the same host process.
//   wait   waits for the host child.
//
// Mach ports are real host ports (see mach.cpp), so a forked child starts
// with none of its parent's, as on a real Mac, and an exec'd program with a
// clean set. The bootstrap port alone is passed down.

#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>

#include "kernel/hooks.h"
#include "kernel/process.h"

namespace {

enum class FileKind { Program, Script, WrongCpu, Other };

u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u32 le32(const u8* p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }

// Looks at the start of a file to see what running it would take. For a "#!"
// script, `words` gets the interpreter and whatever follows it on the line.
FileKind probe(const std::string& host, std::vector<std::string>& words) {
  u8 head[1024] = {};
  const int fd = open(host.c_str(), O_RDONLY);
  if (fd < 0) return FileKind::Other;
  const ssize_t n = read(fd, head, sizeof head - 1);
  close(fd);
  if (n < 4) return FileKind::Other;

  if (head[0] == '#' && head[1] == '!') {
    std::string word;
    for (ssize_t i = 2; i <= n; i++) {
      const char c = i < n ? (char)head[i] : '\n';
      if (c == ' ' || c == '\t' || c == '\n') {
        if (!word.empty()) words.push_back(word);
        word.clear();
        if (c == '\n') break;
      } else {
        word += c;
      }
    }
    return words.empty() ? FileKind::Other : FileKind::Script;
  }

  const u32 kPowerPC = 18, kI386 = 7;
  const u32 magic = be32(head);
  if (magic == 0xFEEDFACE) return be32(head + 4) == kPowerPC ? FileKind::Program : FileKind::WrongCpu;
  if (magic == 0xCEFAEDFE) return le32(head + 4) == kI386 ? FileKind::Program : FileKind::WrongCpu;
  if (magic == 0xFEEDFACF || magic == 0xCFFAEDFE) return FileKind::WrongCpu;  // 64-bit
  if (magic == 0xCAFEBABE) {
    const u32 count = be32(head + 4);
    for (u32 i = 0; i < count && 8 + (i + 1) * 20 <= (u32)n; i++) {
      const u32 cpu = be32(head + 8 + i * 20);
      if (cpu == kPowerPC || cpu == kI386) return FileKind::Program;
    }
    return FileKind::WrongCpu;
  }
  return FileKind::Other;
}

}  // namespace

SysRet Process::two_results(u32 first, u32 second) const {
  // SysRet::Value64 puts the high half in the first register on PowerPC and
  // in the second on Intel.
  return SysRet::ok64(arch_ == Arch::PPC ? ((u64)first << 32) | second
                                         : ((u64)second << 32) | first);
}

SysRet Process::sys_fork(Thread& t) {
  // The child may want system services, and will inherit only a right to
  // send to the bootstrap port: someone has to be answering it.
  ensure_session_server();
  int ready[2];
  if (pipe(ready) != 0) return SysRet::err(errno);
  if (fcntl(ready[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(ready[1], F_SETFD, FD_CLOEXEC) < 0) {
    const int error = errno;
    close(ready[0]);
    close(ready[1]);
    return SysRet::err(error);
  }
  // Hold the kernel lock across the fork so the child doesn't inherit it
  // locked by a thread that no longer exists there.
  std::unique_lock<std::mutex> lk(kmu_);
  const pid_t parent = getpid();
  lock_for_fork(true);
  const pid_t pid = fork();
  lock_for_fork(false);
  if (pid < 0) {
    const int error = errno;
    close(ready[0]);
    close(ready[1]);
    return SysRet::err(error);
  }
  if (pid == 0) {
    close(ready[0]);
    // Only the forking thread exists in the child.
    for (const auto& other : threads_) {
      if (other.get() != &t) other->done = true;
    }
    if (&t != &main_) {
      main_.done = true;
      sole_thread_ = &t;
    }
    forget_pending_signals();  // they were the parent's
    panel_fork_child();        // as was any wait for the file picker
    // Nor does a child get its parent's Mach ports, beyond the bootstrap
    // port: the host kernel gave this process a fresh, empty set. Its
    // semaphores and message queues are gone, and the C library, which knows
    // that, sets itself up afresh.
    lk.unlock();
    setup_ports(t);
    const char registered = 1;
    ssize_t sent;
    do {
      sent = write(ready[1], &registered, 1);
    } while (sent < 0 && errno == EINTR);
    if (sent != 1) log_warn("could not report the forked task port to the parent: %s", strerror(errno));
    close(ready[1]);
    t.host_port = pthread_mach_thread_np(pthread_self());
    // Mac OS X reports "I am the child" in the second result register; the C
    // library turns that into the familiar return value of zero.
    return two_results((u32)parent, 1);
  }
  close(ready[1]);
  lk.unlock();
  char registered;
  ssize_t got;
  do {
    got = read(ready[0], &registered, 1);
  } while (got < 0 && errno == EINTR);
  close(ready[0]);
  if (got != 1) log_warn("forked process %d did not finish setting up its task port", (int)pid);
  return two_results((u32)pid, 0);
}

SysRet Process::sys_execve(const std::string& path, u32 argv, u32 envp,
                           std::optional<Arch> prefer) {
  if (opt_.self_path.empty()) return SysRet::err(ENOSYS);
  // Reads a null-terminated array of string pointers.
  auto strings = [&](u32 array, std::vector<std::string>& out) -> bool {
    for (u32 i = 0; array && i < 65536; i++) {
      if (!mem_.range_ok(array + 4 * i, 4)) return false;
      const u32 p = mem_.r32(array + 4 * i);
      if (!p) return true;
      std::string s;
      if (!mem_.read_cstr(p, s)) return false;
      out.push_back(s);
    }
    return true;
  };
  std::vector<std::string> args, env;
  if (!strings(argv, args) || !strings(envp, env)) return SysRet::err(EFAULT);

  // Find the program to run: the file itself, or for a "#!" script the
  // interpreter its first line names, which is then handed the script.
  std::string program = path, host;
  for (int depth = 0;; depth++) {
    host = host_path(program);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return SysRet::err(errno);
    if (!S_ISREG(st.st_mode) || access(host.c_str(), X_OK) != 0) return SysRet::err(EACCES);
    std::vector<std::string> words;
    const FileKind kind = probe(host, words);
    if (kind == FileKind::Program) break;
    if (kind == FileKind::WrongCpu) return SysRet::err(86);  // EBADARCH
    if (kind == FileKind::Other || depth == 4) return SysRet::err(ENOEXEC);
    words.push_back(program);
    if (args.empty()) args.push_back(program);
    args.erase(args.begin());
    args.insert(args.begin(), words.begin(), words.end());
    program = words[0];
  }

  // cocaine's own options first, then the guest program and its arguments.
  const Arch cpu = prefer ? *prefer : arch_;
  std::vector<std::string> cmd = {opt_.self_path, "--prefer-arch", arch_name(cpu)};
  if (!opt_.sysroot.empty()) {
    cmd.push_back("--sysroot");
    cmd.push_back(opt_.sysroot);
  }
  if (opt_.strace) cmd.push_back("--strace");
  if (opt_.as_root) cmd.push_back("--as-root");
  if (!session_name_.empty()) {
    cmd.push_back("--session");
    cmd.push_back(session_name_);
  }
  if (g_verbosity > 0) cmd.push_back(g_verbosity > 1 ? "-vv" : "-v");
  if (!args.empty()) {
    cmd.push_back("--argv0");
    cmd.push_back(args[0]);
  }
  // The guest's DYLD_ variables are for the guest's dyld, not this Mac's.
  std::vector<std::string> host_env;
  for (const std::string& e : env) {
    if (e.compare(0, 5, "DYLD_") == 0) {
      cmd.push_back("--env");
      cmd.push_back(e);
    } else {
      host_env.push_back(e);
    }
  }
  cmd.push_back("--");
  cmd.push_back(host);
  for (size_t i = 1; i < args.size(); i++) cmd.push_back(args[i]);

  std::vector<char*> c_cmd, c_env;
  for (std::string& s : cmd) c_cmd.push_back(s.data());
  for (std::string& s : host_env) c_env.push_back(s.data());
  c_cmd.push_back(nullptr);
  c_env.push_back(nullptr);
  execve(opt_.self_path.c_str(), c_cmd.data(), c_env.data());
  return SysRet::err(errno);  // only reached if it failed
}

// posix_spawn: fork and exec in one call, with a list of things to do to the
// child's open files in between. Launch Services starts applications this
// way, naming the CPU it wants a universal program to run on.
SysRet Process::sys_posix_spawn(u32 pid_out, const std::string& path, u32 desc, u32 argv,
                                u32 envp) {
  enum : u32 { kSetPgroup = 0x02, kSetExec = 0x40, kStartSuspended = 0x80 };
  constexpr u32 kActionSize = 1040, kActionPath = 14;

  u32 attr = 0, attr_size = 0, actions = 0, actions_size = 0;
  if (desc) {
    if (!mem_.range_ok(desc, 16)) return SysRet::err(EFAULT);
    attr_size = mem_.r32(desc);
    attr = mem_.r32(desc + 4);
    actions_size = mem_.r32(desc + 8);
    actions = mem_.r32(desc + 12);
  }
  u32 flags = 0;
  s32 pgroup = 0;
  std::optional<Arch> prefer;
  if (attr && attr_size >= 32) {
    if (!mem_.range_ok(attr, attr_size)) return SysRet::err(EFAULT);
    flags = mem_.r16(attr);
    pgroup = (s32)mem_.r32(attr + 12);
    for (u32 i = 0; i < 4 && !prefer; i++) {
      const u32 cpu = mem_.r32(attr + 16 + 4 * i);
      if (cpu == 18) prefer = Arch::PPC;
      if (cpu == 7) prefer = Arch::I386;
    }
  }
  u32 count = 0;
  if (actions && actions_size >= 8) {
    if (!mem_.range_ok(actions, actions_size)) return SysRet::err(EFAULT);
    count = mem_.r32(actions + 4);
    if ((u64)8 + (u64)count * kActionSize > actions_size) return SysRet::err(EINVAL);
  }
  if (flags & kStartSuspended) log_warn("posix_spawn: starting suspended is not supported");

  // Fail here, where the caller can be told, if the program plainly can't run.
  if (access(host_path(path).c_str(), X_OK) != 0) return SysRet::err(errno);

  pid_t pid = 0;
  if (!(flags & kSetExec)) {
    ensure_session_server();
    std::unique_lock<std::mutex> lk(kmu_);
    lock_for_fork(true);
    pid = fork();
    lock_for_fork(false);
    if (pid < 0) return SysRet::err(errno);
  }
  if (pid == 0) {
    bool ok = true;
    if (flags & kSetPgroup) setpgid(0, pgroup);
    for (u32 i = 0; i < count && ok; i++) {
      const u32 act = actions + 8 + i * kActionSize;
      const u32 type = mem_.r32(act);
      const int fd = (int)mem_.r32(act + 4);
      const int other = (int)mem_.r32(act + 8);  // open flags, or dup2's target
      if (type == 0) {  // open
        std::string file;
        mem_.read_cstr(act + kActionPath, file);
        const int got = open(host_path(file).c_str(), other, mem_.r16(act + 12));
        ok = got >= 0;
        if (ok && got != fd) {
          ok = dup2(got, fd) >= 0;
          close(got);
        }
      } else if (type == 1) {  // close
        close(fd);
      } else if (type == 2) {  // dup2
        ok = dup2(fd, other) >= 0;
      }
    }
    const SysRet r = ok ? sys_execve(path, argv, envp, prefer) : SysRet::err(errno);
    if (flags & kSetExec) return r;
    _exit(127);
  }
  if (pid_out && mem_.range_ok(pid_out, 4)) mem_.w32(pid_out, (u32)pid);
  return SysRet::ok();
}

// Named shared memory. This Mac has its own objects under the same names
// (its notification daemon's, for one), so the guests' live apart from them:
// as files in a folder belonging to the session, which its server removes
// when the session ends.
SysRet Process::sys_shm(u32 num, const std::string& name, int oflag, u32 mode) {
  ensure_session_server();  // sharing needs a session to share within
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) strlcpy(tmp, "/tmp/", sizeof tmp);
  std::string dir = tmp;
  {
    std::lock_guard<std::mutex> lk(session_mu_);
    dir += session_name_.empty() ? "cocaine.session." + std::to_string(getpid()) : session_name_;
  }
  mkdir(dir.c_str(), 0700);
  dir += "/shm";
  mkdir(dir.c_str(), 0700);
  std::string file;
  for (char c : name) file += c == '/' ? '_' : c;
  if (file.empty() || file.size() > 200) return SysRet::err(EINVAL);
  const std::string path = dir + "/" + file;
  if (num == 267) return unlink(path.c_str()) == 0 ? SysRet::ok() : SysRet::err(errno);
  const int fd = open(path.c_str(), oflag, (mode_t)mode);
  return fd < 0 ? SysRet::err(errno) : SysRet::ok((u32)fd);
}

SysRet Process::sys_wait4(int pid, u32 status, int options, u32 rusage) {
  int st = 0;
  struct rusage ru;
  const pid_t got = wait4(pid, &st, options, &ru);
  if (got < 0) return SysRet::err(errno);
  if (status && mem_.range_ok(status, 4)) mem_.w32(status, (u32)st);
  if (rusage && mem_.range_ok(rusage, 72)) {
    // The 32-bit layout: two 8-byte times, then fourteen 32-bit counters.
    memset(mem_.host(rusage), 0, 72);
    mem_.w32(rusage, (u32)ru.ru_utime.tv_sec);
    mem_.w32(rusage + 4, (u32)ru.ru_utime.tv_usec);
    mem_.w32(rusage + 8, (u32)ru.ru_stime.tv_sec);
    mem_.w32(rusage + 12, (u32)ru.ru_stime.tv_usec);
    mem_.w32(rusage + 16, (u32)(ru.ru_maxrss / 1024));
  }
  return SysRet::ok((u32)got);
}
