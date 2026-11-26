// The BSD half of the fake kernel: Mac OS X system calls answered on the host.
//
// The host is also Darwin, so most calls map straight onto the same host
// call. What differs is the calling convention (how arguments arrive and how
// results and errors go back), structure layouts (32-bit fields where the
// host has 64) and, for PowerPC, byte order.

#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <sys/event.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#include "kernel/guest_struct.h"
#include "kernel/process.h"

namespace {

// Names for the BSD system calls of the Mac OS X 10.5 era, for logs.
const char* bsd_name(u32 n) {
  switch (n) {
    case 1: return "exit";
    case 2: return "fork";
    case 3: return "read";
    case 4: return "write";
    case 5: return "open";
    case 6: return "close";
    case 7: return "wait4";
    case 10: return "unlink";
    case 12: return "chdir";
    case 20: return "getpid";
    case 24: return "getuid";
    case 25: return "geteuid";
    case 33: return "access";
    case 37: return "kill";
    case 39: return "getppid";
    case 41: return "dup";
    case 42: return "pipe";
    case 43: return "getegid";
    case 46: return "sigaction";
    case 47: return "getgid";
    case 48: return "sigprocmask";
    case 53: return "sigaltstack";
    case 54: return "ioctl";
    case 58: return "readlink";
    case 59: return "execve";
    case 73: return "munmap";
    case 79: return "getgroups";
    case 74: return "mprotect";
    case 75: return "madvise";
    case 90: return "dup2";
    case 92: return "fcntl";
    case 93: return "select";
    case 18: return "getfsstat";
    case 26: return "ptrace";
    case 97: return "socket";
    case 98: return "connect";
    case 133: return "sendto";
    case 116: return "gettimeofday";
    case 117: return "getrusage";
    case 120: return "readv";
    case 121: return "writev";
    case 13: return "fchdir";
    case 153: return "pread";
    case 154: return "pwrite";
    case 157: return "statfs";
    case 158: return "fstatfs";
    case 169: return "csops";
    case 188: return "stat";
    case 189: return "fstat";
    case 190: return "lstat";
    case 194: return "getrlimit";
    case 196: return "getdirentries";
    case 197: return "mmap";
    case 199: return "lseek";
    case 202: return "sysctl";
    case 220: return "getattrlist";
    case 221: return "setattrlist";
    case 223: return "exchangedata";
    case 234: return "getxattr";
    case 237: return "fsetxattr";
    case 240: return "listxattr";
    case 279: return "stat_extended";
    case 280: return "lstat_extended";
    case 281: return "fstat_extended";
    case 282: return "chmod_extended";
    case 283: return "fchmod_extended";
    case 284: return "access_extended";
    case 341: return "stat64_extended";
    case 342: return "lstat64_extended";
    case 343: return "fstat64_extended";
    case 294: return "shared_region_check_np";
    case 327: return "issetugid";
    case 338: return "stat64";
    case 339: return "fstat64";
    case 340: return "lstat64";
    case 344: return "getdirentries64";
    case 345: return "statfs64";
    case 346: return "fstatfs64";
    case 334: return "semwait_signal";
    case 360: return "bsdthread_create";
    case 361: return "bsdthread_terminate";
    case 366: return "bsdthread_register";
    case 396: return "read_nocancel";
    case 397: return "write_nocancel";
    case 398: return "open_nocancel";
    case 399: return "close_nocancel";
    case 406: return "fcntl_nocancel";
    case 414: return "pread_nocancel";
    case 230: return "poll";
    case 417: return "poll_nocancel";
    default: return "?";
  }
}

constexpr u32 kMapShared = 0x0001;
constexpr u32 kMapFixed = 0x0010;
constexpr u32 kMapAnon = 0x1000;

SysRet result(long r) { return r < 0 ? SysRet::err(errno) : SysRet::ok((u32)r); }

}  // namespace

// PowerPC: the call number is in r0 and arguments in r3-r10. A successful
// call returns to the instruction after the one following `sc`; a failed one
// returns right after `sc` with the error number in r3. Negative numbers are
// Mach traps, which just return a value in r3.
void Process::ppc_syscall(Thread& t) {
  PPCState& s = t.ppc;
  SysArgs a;
  a.big_endian = true;
  for (int i = 0; i < 8; i++) a.w[i] = s.gpr[3 + i];

  const s32 r0 = (s32)s.gpr[0];
  if (r0 < 0) {
    s.gpr[3] = mach_trap(t, (u32)-r0, a);
    return;
  }
  if (r0 >= 0x6000) {
    ppc_fast_trap(t, (u32)r0);
    return;
  }

  u32 num = (u32)r0;
  if (num == 0) num = a.u();  // syscall(number, ...)
  const SysRet r = bsd_syscall(t, num, a);
  if (r.kind == SysRet::Error && r.value == EINTR && restart_after_signal(num)) {
    // The handler runs first, then the call again: nothing has been touched,
    // so backing up to the `sc` is all it takes.
    s.pc -= 4;
    return;
  }
  switch (r.kind) {
    case SysRet::Value:
      s.gpr[3] = (u32)r.value;
      s.pc += 4;
      break;
    case SysRet::Value64:
      s.gpr[3] = (u32)(r.value >> 32);
      s.gpr[4] = (u32)r.value;
      s.pc += 4;
      break;
    case SysRet::Error:
      s.gpr[3] = (u32)r.value;
      break;
    case SysRet::NoReturn:
      break;
  }
}

// PowerPC-only traps: `sc` with r0 at 0x6000 or above. They return like Mach
// traps, without the skipped instruction.
void Process::ppc_fast_trap(Thread& t, u32 num) {
  PPCState& s = t.ppc;
  switch (num) {
    case 0x7FF1:  // set this thread's pthread pointer; user code reads it back as SPRG3
      s.sprg3 = s.gpr[3];
      break;
    case 0x7FF2:  // get it
      s.gpr[3] = s.sprg3;
      break;
    case 0x7FF3:  // which optional CPU state is in use: floating point (1) and AltiVec (2)
      s.gpr[3] = 3;
      break;
    default:
      log_warn("unimplemented PowerPC special trap 0x%x at 0x%08x", num, s.pc - 4);
      s.gpr[3] = (u32)-1;
      break;
  }
  if (opt_.strace) std::fprintf(stderr, "[ppc trap 0x%x] = 0x%x\n", num, s.gpr[3]);
}

// Intel: the call number is in eax and arguments sit on the stack above a
// return-address slot. Errors come back with the carry flag set and the error
// number in eax. `int 0x80` is BSD, `int 0x81` Mach, `int 0x82` machine
// dependent; `sysenter` carries both kinds (negative eax means Mach), with the
// user stack pointer in ecx and the return address in edx.
void Process::x86_syscall(Thread& t, bool sysenter, u8 vector) {
  X86State& s = t.x86;
  u32* const R = s.reg;
  const u32 arg_base = (sysenter ? R[X86State::ECX] : R[X86State::ESP]) + 4;

  SysArgs a;
  if (mem_.range_ok(arg_base, sizeof a.w)) {
    for (int i = 0; i < 12; i++) a.w[i] = mem_.r32(arg_base + 4 * (u32)i);
  }

  const s32 eax = (s32)R[X86State::EAX];
  const bool is_mach = sysenter ? eax < 0 : vector == 0x81;
  const u32 entry_eip = s.eip - 2, entry_esp = R[X86State::ESP];  // both instructions are two bytes
  if (sysenter) {
    s.eip = R[X86State::EDX];
    R[X86State::ESP] = R[X86State::ECX];
  }

  if (is_mach) {
    R[X86State::EAX] = mach_trap(t, (u32)-eax, a);
    return;
  }
  if (!sysenter && vector == 0x82) {
    R[X86State::EAX] = machdep_call(t, (u32)eax, a);
    return;
  }
  if (!sysenter && vector != 0x80) {
    log_warn("unimplemented Intel trap: int 0x%02x with eax=0x%x", vector, (u32)eax);
    R[X86State::EAX] = (u32)-1;
    return;
  }

  // The top half of eax says how many bytes of arguments there are; the
  // number is the bottom half.
  u32 num = (u32)eax & 0xFFFF;
  if (num == 0) num = a.u();  // syscall(number, ...)
  const SysRet r = bsd_syscall(t, num, a);
  if (r.kind == SysRet::Error && r.value == EINTR && restart_after_signal(num)) {
    // The handler runs first, then the call again, from its own instruction.
    s.eip = entry_eip;
    R[X86State::ESP] = entry_esp;
    return;
  }
  switch (r.kind) {
    case SysRet::Value:
      R[X86State::EAX] = (u32)r.value;
      s.eflags &= ~1u;
      break;
    case SysRet::Value64:
      R[X86State::EAX] = (u32)r.value;
      R[X86State::EDX] = (u32)(r.value >> 32);
      s.eflags &= ~1u;
      break;
    case SysRet::Error:
      R[X86State::EAX] = (u32)r.value;
      s.eflags |= 1u;
      break;
    case SysRet::NoReturn:
      break;
  }
}

// Intel machine-dependent calls (`int 0x82`).
u32 Process::machdep_call(Thread& t, u32 num, SysArgs& a) {
  X86State& s = t.x86;
  u32 r;
  switch (num) {
    case 2:  // thread_get_cthread_self
      r = s.tls_base;
      break;
    case 3:  // thread_fast_set_cthread_self: point %gs at this thread's data
      // The kernel loads %gs itself; the C library doesn't touch the
      // register afterwards, it just starts using %gs-relative addresses.
      s.tls_base = a.u();
      s.tls_selector = 0x37;
      s.seg[X86State::GS] = s.tls_selector;
      s.seg_base[X86State::GS] = s.tls_base;
      r = s.tls_selector;
      break;
    default:
      log_warn("unimplemented Intel machine-dependent call %u", num);
      r = (u32)-1;
      break;
  }
  if (opt_.strace) std::fprintf(stderr, "[machdep %u] = 0x%x\n", num, r);
  return r;
}

SysRet Process::sys_mmap(u32 addr, u32 len, u32 prot, u32 flags, s32 fd, u64 offset) {
  if (!len) return SysRet::err(EINVAL);
  const u64 size = ((u64)len + 0xFFF) & ~0xFFFull;
  u32 at;
  if ((flags & kMapShared) && !(flags & (kMapAnon | kMapFixed))) {
    // Memory really shared with whoever else maps the file. Possible when
    // the mapping can sit on the host's own (larger) page boundaries, which
    // it can whenever the guest lets the kernel pick the address.
    at = addr ? mem_.map_shared_file(len, (u8)(prot & 7), fd, offset, addr, 0xC0000000u) : 0;
    if (!at) at = mem_.map_shared_file(len, (u8)(prot & 7), fd, offset, 0x1000, 0xC0000000u);
    if (at) return SysRet::ok(at);
    if (prot & 2) {
      log_warn("a shared file mapping could not be shared; changes made through it stay private");
    }
  }
  if (flags & kMapFixed) {
    if ((addr & 0xFFF) || (u64)addr + size > GuestMemory::kSpan) return SysRet::err(EINVAL);
    at = addr;
  } else {
    // Like the real kernel: lowest free range at or above the hint, else
    // anywhere above the zero page.
    at = addr ? mem_.find_free(size, addr, 0xC0000000u) : 0;
    if (!at) at = mem_.find_free(size, 0x1000, 0xC0000000u);
    if (!at) return SysRet::err(ENOMEM);
  }
  if (!mem_.map(at, size, (u8)(prot & 7))) return SysRet::err(ENOMEM);

  if (!(flags & kMapAnon)) {
    // Other file mappings are private copies: guest pages are 4 KB and host
    // pages 16 KB, so the host can't map the file in place.
    u64 done = 0;
    while (done < len) {
      const ssize_t n = pread(fd, mem_.host(at) + done, len - done, (off_t)(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        const int e = errno;
        mem_.unmap(at, size);
        return SysRet::err(e);
      }
      if (n == 0) break;  // past end of file: the rest stays zero
      done += (u64)n;
    }
    if (prot & 4) note_code_mapping(at, len, fd, offset);  // a library's code
  }
  return SysRet::ok(at);
}

// The 32-bit layouts of struct stat: the original one, and the "64-bit inode"
// one that the stat64 calls return.
void Process::write_stat(u32 buf, const struct stat& st, bool wide) {
  GuestWriter w(mem_, buf);
  w.u32_((u32)st.st_dev);
  if (wide) {
    w.u16_(st.st_mode);
    w.u16_(st.st_nlink);
    w.u64_(st.st_ino);
  } else {
    w.u32_((u32)st.st_ino);
    w.u16_(st.st_mode);
    w.u16_(st.st_nlink);
  }
  // A daemon told it is root finds the files it made (really ours) to be
  // root's, as it expects of them.
  const bool ours = opt_.as_root && st.st_uid == getuid();
  w.u32_(ours ? 0 : st.st_uid);
  w.u32_(ours ? 0 : st.st_gid);
  w.u32_((u32)st.st_rdev);
  w.u32_((u32)st.st_atimespec.tv_sec);
  w.u32_((u32)st.st_atimespec.tv_nsec);
  w.u32_((u32)st.st_mtimespec.tv_sec);
  w.u32_((u32)st.st_mtimespec.tv_nsec);
  w.u32_((u32)st.st_ctimespec.tv_sec);
  w.u32_((u32)st.st_ctimespec.tv_nsec);
  if (wide) {
    w.u32_((u32)st.st_birthtimespec.tv_sec);
    w.u32_((u32)st.st_birthtimespec.tv_nsec);
  }
  w.u64_((u64)st.st_size);
  w.u64_((u64)st.st_blocks);
  w.u32_((u32)st.st_blksize);
  w.u32_(st.st_flags);
  w.u32_(st.st_gen);
  w.zero(4 + 16);  // spare fields
}

SysRet Process::sys_stat(const std::string& path, u32 buf, bool follow, bool wide) {
  struct stat st;
  const std::string host = host_path(path, follow);
  if ((follow ? stat(host.c_str(), &st) : lstat(host.c_str(), &st)) != 0) return SysRet::err(errno);
  if (!mem_.range_ok(buf, wide ? 108 : 96)) return SysRet::err(EFAULT);
  write_stat(buf, st, wide);
  return SysRet::ok();
}

SysRet Process::sys_fstat(int fd, u32 buf, bool wide) {
  struct stat st;
  if (fstat(fd, &st) != 0) return SysRet::err(errno);
  if (!mem_.range_ok(buf, wide ? 108 : 96)) return SysRet::err(EFAULT);
  write_stat(buf, st, wide);
  return SysRet::ok();
}

SysRet Process::sys_fcntl(int fd, u32 cmd, u32 arg) {
  switch (cmd) {
    case 0:   // F_DUPFD
    case 1:   // F_GETFD
    case 2:   // F_SETFD
    case 3:   // F_GETFL
    case 4:   // F_SETFL
      return result(fcntl(fd, (int)cmd, (int)arg));
    case 45:  // F_RDAHEAD
    case 48:  // F_NOCACHE
    case 51:  // F_FULLFSYNC
      return result(fcntl(fd, (int)cmd, (int)arg));
    case 44:  // F_RDADVISE: a read-ahead hint, with nothing to do here
      return SysRet::ok();
    case 50: {  // F_GETPATH: the file's path, as the guest would spell it
      char path[PATH_MAX];
      if (fcntl(fd, F_GETPATH, path) != 0) return SysRet::err(errno);
      const std::string g = guest_path(path);
      if (!mem_.copy_out(arg, g.c_str(), g.size() + 1)) return SysRet::err(EFAULT);
      return SysRet::ok();
    }
    case 42: {  // F_PREALLOCATE
      if (!mem_.range_ok(arg, 32)) return SysRet::err(EFAULT);
      fstore_t store = {};
      store.fst_flags = mem_.r32(arg);
      store.fst_posmode = (int)mem_.r32(arg + 4);
      store.fst_offset = (off_t)mem_.r64(arg + 8);
      store.fst_length = (off_t)mem_.r64(arg + 16);
      if (fcntl(fd, F_PREALLOCATE, &store) != 0) return SysRet::err(errno);
      mem_.w64(arg + 24, (u64)store.fst_bytesalloc);
      return SysRet::ok();
    }
    case 7:
    case 8:
    case 9: {  // F_GETLK, F_SETLK, F_SETLKW: record locks, which databases live by
      // The record: start and length (64 bits each), the owner's pid, then
      // the kind of lock and what the start is measured from, 16 bits each.
      if (!mem_.range_ok(arg, 24)) return SysRet::err(EFAULT);
      struct flock fl = {};
      fl.l_start = (off_t)mem_.r64(arg);
      fl.l_len = (off_t)mem_.r64(arg + 8);
      fl.l_pid = (pid_t)mem_.r32(arg + 16);
      fl.l_type = (short)mem_.r16(arg + 20);
      fl.l_whence = (short)mem_.r16(arg + 22);
      if (fcntl(fd, (int)cmd, &fl) != 0) return SysRet::err(errno);
      if (cmd == 7) {
        mem_.w64(arg, (u64)fl.l_start);
        mem_.w64(arg + 8, (u64)fl.l_len);
        mem_.w32(arg + 16, (u32)fl.l_pid);
        mem_.w16(arg + 20, (u16)fl.l_type);
        mem_.w16(arg + 22, (u16)fl.l_whence);
      }
      return SysRet::ok();
    }
    default:
      log_warn("unimplemented fcntl command %u", cmd);
      return SysRet::err(EINVAL);
  }
}

// The host's raw directory read. It is not in any header, but it is what
// readdir() is built on.
extern "C" ssize_t __getdirentries64(int fd, void* buf, size_t bufsize, off_t* basep);

// Reads directory entries and rewrites them in one of the guest's layouts:
//   original: u32 inode, u16 record length, u8 type, u8 name length, name
//   "64":     u64 inode, u64 seek offset, u16 record length, u16 name length,
//             u8 type, name
// Both are padded to a multiple of four bytes.
SysRet Process::sys_getdirentries(int fd, u32 buf, u32 nbytes, u32 basep, bool wide) {
  if (!mem_.range_ok(buf, nbytes)) return SysRet::err(EFAULT);
  // Room for the biggest host record there is, a name of 1024 bytes.
  constexpr u32 kRoom = 1048;
  std::vector<u8> host(nbytes < kRoom ? kRoom : nbytes);

  // Rewrites host records [0, got) into the guest's buffer, stopping at the
  // first that doesn't fit. Returns how many host bytes were used up.
  u32 out = 0;
  auto convert = [&](ssize_t got) {
    out = 0;
    ssize_t off = 0;
    while (off + 21 <= got) {
      // Host record: u64 inode, u64 seek offset, u16 length, u16 name length,
      // u8 type, name.
      u64 ino, seekoff;
      u16 reclen, namlen;
      memcpy(&ino, host.data() + off, 8);
      memcpy(&seekoff, host.data() + off + 8, 8);
      memcpy(&reclen, host.data() + off + 16, 2);
      memcpy(&namlen, host.data() + off + 18, 2);
      const u8 type = host[(size_t)off + 20];
      const char* name = (const char*)host.data() + off + 21;
      if (!reclen || reclen > got - off) break;
      if (ino != 0 && namlen < (wide ? 1024 : 256)) {
        const u32 fixed = wide ? 21 : 8;
        const u32 size = (fixed + namlen + 1 + 3) & ~3u;
        if (out + size > nbytes) break;
        GuestWriter w(mem_, buf + out);
        if (wide) {
          w.u64_(ino);
          w.u64_(seekoff);
          w.u16_((u16)size);
          w.u16_(namlen);
          w.u8_(type);
        } else {
          w.u32_((u32)ino);
          w.u16_((u16)size);
          w.u8_(type);
          w.u8_((u8)namlen);
        }
        memcpy(mem_.host(w.at()), name, namlen);
        memset(mem_.host(w.at() + namlen), 0, size - fixed - namlen);
        out += size;
      }
      off += reclen;
    }
    return off;
  };

  // Reads from the host into a cleared buffer, so that nothing left from an
  // earlier read is taken for a record.
  off_t base = 0;
  auto read_host = [&](size_t size) {
    memset(host.data(), 0, size);
    return __getdirentries64(fd, host.data(), size, &base);
  };

  // A guest record is never bigger than the host record it came from, so
  // asking the host for no more than the guest's buffer holds means all of
  // it fits, and the directory is left just past what the guest gets.
  const off_t start = lseek(fd, 0, SEEK_CUR);
  ssize_t got = read_host(nbytes);
  ssize_t used = got > 0 ? convert(got) : 0;
  if (nbytes < kRoom && ((got < 0 && errno == EINVAL) || (got > 0 && !used))) {
    // The host turned the small buffer down, as it does when its next
    // record is too big for it (APFS instead claims to have filled the
    // buffer and puts nothing there). The guest's smaller record may still
    // fit: read again, from the same place, with room to spare.
    if (start < 0 || lseek(fd, start, SEEK_SET) < 0) return SysRet::err(EINVAL);
    got = read_host(kRoom);
    used = got > 0 ? convert(got) : 0;
  }
  if (got < 0) return SysRet::err(errno);
  if (used < got) {
    // Not everything read fits. The directory has moved past all of it, so
    // go back and read again only as far as what fits, leaving the rest for
    // the next call. If even that can't be done, nothing is returned and
    // the directory stays where it was.
    if (!used || start < 0 || lseek(fd, start, SEEK_SET) < 0) {
      if (start >= 0) lseek(fd, start, SEEK_SET);
      return SysRet::err(EINVAL);
    }
    got = read_host((size_t)used);
    if (got <= 0 || convert(got) != got) {
      const int e = got < 0 ? errno : EINVAL;
      lseek(fd, start, SEEK_SET);
      return SysRet::err(e);
    }
  }
  if (basep && wide && mem_.range_ok(basep, 8)) mem_.w64(basep, (u64)base);
  if (basep && !wide && mem_.range_ok(basep, 4)) mem_.w32(basep, (u32)base);
  return SysRet::ok(out);
}

// struct statfs in its two 32-bit layouts: the original one with 32-bit
// counts, and the "64" one, which is the layout the host itself uses today.
SysRet Process::sys_statfs(const struct statfs& fs, u32 buf, bool wide) {
  if (wide) {
    if (!mem_.range_ok(buf, 2168)) return SysRet::err(EFAULT);
    memset(mem_.host(buf), 0, 2168);
    GuestWriter w(mem_, buf);
    w.u32_(fs.f_bsize);
    w.u32_((u32)fs.f_iosize);
    w.u64_(fs.f_blocks);
    w.u64_(fs.f_bfree);
    w.u64_(fs.f_bavail);
    w.u64_(fs.f_files);
    w.u64_(fs.f_ffree);
    w.u32_((u32)fs.f_fsid.val[0]);
    w.u32_((u32)fs.f_fsid.val[1]);
    w.u32_(fs.f_owner);
    w.u32_(17);  // file system type number: HFS
    w.u32_(fs.f_flags & 0xFFFF);
    w.u32_(0);
    memcpy(mem_.host(buf + 72), "hfs", 4);
    const std::string on = guest_path(fs.f_mntonname);
    memcpy(mem_.host(buf + 88), on.c_str(), on.size() < 1023 ? on.size() : 1023);
    memcpy(mem_.host(buf + 1112), fs.f_mntfromname, strnlen(fs.f_mntfromname, 1023));
    return SysRet::ok();
  }
  if (!mem_.range_ok(buf, 272)) return SysRet::err(EFAULT);
  memset(mem_.host(buf), 0, 272);
  GuestWriter w(mem_, buf);
  w.u16_(0);  // old type and flags
  w.u16_(0);
  w.u32_(fs.f_bsize);
  w.u32_((u32)fs.f_iosize);
  // Counts are 32-bit here; a modern disk overflows them, so clamp.
  auto clamp = [](u64 v) -> u32 { return v > 0x7FFFFFFFull ? 0x7FFFFFFFu : (u32)v; };
  w.u32_(clamp(fs.f_blocks));
  w.u32_(clamp(fs.f_bfree));
  w.u32_(clamp(fs.f_bavail));
  w.u32_(clamp(fs.f_files));
  w.u32_(clamp(fs.f_ffree));
  w.u32_((u32)fs.f_fsid.val[0]);
  w.u32_((u32)fs.f_fsid.val[1]);
  w.u32_(fs.f_owner);
  w.u16_(0);
  w.u16_(17);  // file system type number: HFS
  w.u32_(fs.f_flags & 0xFFFF);
  w.zero(8);
  // The guest only knows the file systems of its day; call everything HFS.
  memcpy(mem_.host(buf + 60), "hfs", 4);
  const std::string on = guest_path(fs.f_mntonname);
  memcpy(mem_.host(buf + 75), on.c_str(), on.size() < 89 ? on.size() : 89);
  const size_t from = strnlen(fs.f_mntfromname, 89);
  memcpy(mem_.host(buf + 165), fs.f_mntfromname, from);
  return SysRet::ok();
}

SysRet Process::bsd_syscall(Thread& t, u32 num, SysArgs& a) {
  if (opt_.strace) {
    const int n = a.next;
    char who[24] = "";
    if (t.number) std::snprintf(who, sizeof who, "{thread %d} ", t.number);
    std::fprintf(stderr, "%s[bsd %u %s](0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", who, num,
                 bsd_name(num), a.w[n], a.w[n + 1], a.w[n + 2], a.w[n + 3], a.w[n + 4], a.w[n + 5]);
  }
  // Reads a path argument; sets `bad` if the pointer is no good.
  bool bad = false;
  auto path_arg = [&]() -> std::string {
    std::string s;
    if (!mem_.read_cstr(a.u(), s)) bad = true;
    if (opt_.strace && !bad) std::fprintf(stderr, " \"%s\"", s.c_str());
    return s;
  };

  SysRet r = SysRet::err(ENOSYS);
  switch (num) {
    case 1: {  // exit
      const int status = a.i() & 0xFF;
      // Another guest thread may be blocked in a host call indefinitely.
      // End the whole host process, without tearing memory down under it.
      report_stats();
      fflush(nullptr);
      _exit(status);
    }

    // ---- files ----
    case 3:
    case 396: {  // read
      const int fd = a.i();
      const u32 buf = a.u(), n = a.u();
      if (!mem_.range_ok(buf, n)) {
        r = SysRet::err(EFAULT);
      } else {
        const int route = mem_.big_endian() ? route_socket(fd) : 0;
        r = route < 0 ? SysRet::err(-route) : route ? sys_route_io(fd, buf, n, false)
                                                   : result(::read(fd, mem_.host(buf), n));
      }
      break;
    }
    case 4:
    case 397: {  // write
      const int fd = a.i();
      const u32 buf = a.u(), n = a.u();
      if (!mem_.range_ok(buf, n)) {
        r = SysRet::err(EFAULT);
      } else {
        const int route = mem_.big_endian() ? route_socket(fd) : 0;
        r = route < 0 ? SysRet::err(-route) : route ? sys_route_io(fd, buf, n, true)
                                                   : result(::write(fd, mem_.host(buf), n));
      }
      break;
    }
    case 153:
    case 414: {  // pread
      const int fd = a.i();
      const u32 buf = a.u(), n = a.u();
      const u64 offset = a.q();
      r = mem_.range_ok(buf, n) ? result(pread(fd, mem_.host(buf), n, (off_t)offset))
                                : SysRet::err(EFAULT);
      break;
    }
    case 154: {  // pwrite
      const int fd = a.i();
      const u32 buf = a.u(), n = a.u();
      const u64 offset = a.q();
      r = mem_.range_ok(buf, n) ? result(pwrite(fd, mem_.host(buf), n, (off_t)offset))
                                : SysRet::err(EFAULT);
      break;
    }
    case 120:
    case 121:
    case 411:
    case 412: {  // readv, writev
      const int fd = a.i();
      const u32 iov = a.u(), count = a.u();
      if (count > 1024 || !mem_.range_ok(iov, count * 8)) {
        r = SysRet::err(EINVAL);
        break;
      }
      std::vector<struct iovec> host(count);
      bool ok = true;
      for (u32 i = 0; i < count; i++) {
        const u32 base = mem_.r32(iov + 8 * i), len = mem_.r32(iov + 8 * i + 4);
        ok &= mem_.range_ok(base, len);
        host[i].iov_base = mem_.host(base);
        host[i].iov_len = len;
      }
      if (!ok) {
        r = SysRet::err(EFAULT);
      } else {
        r = result((num == 120 || num == 411) ? readv(fd, host.data(), (int)count)
                              : writev(fd, host.data(), (int)count));
      }
      break;
    }
    case 5:
    case 398: {  // open
      const std::string path = path_arg();
      const int flags = a.i(), mode = a.i();
      if (bad) {
        r = SysRet::err(EFAULT);
      } else if (path == "/dev/dtracehelper") {
        // This Mac has the device, but it speaks a newer protocol than the
        // guest's dyld. Without it, dyld quietly skips DTrace registration.
        r = SysRet::err(ENOENT);
      } else {
        const std::string host = host_path(path);
        int fd = ::open(host.c_str(), flags, mode);
        if (fd < 0 && errno == ENOENT) {
          fd = open_resolver_config(host, flags);
          if (fd < 0 && errno == ENOENT) fd = open_appledouble_fork(host, flags);
        }
        r = result(fd);
      }
      break;
    }
    case 6:
    case 399:  // close
      r = result(::close(a.i()));
      break;
    case 199: {  // lseek
      const int fd = a.i();
      const s64 offset = (s64)a.q();
      const int whence = a.i();
      const off_t pos = lseek(fd, offset, whence);
      r = pos < 0 ? SysRet::err(errno) : SysRet::ok64((u64)pos);
      break;
    }
    case 41: r = result(dup(a.i())); break;
    case 90: {
      const int from = a.i(), to = a.i();
      r = result(dup2(from, to));
      break;
    }
    case 92:
    case 406: {  // fcntl
      const int fd = a.i();
      const u32 cmd = a.u(), arg = a.u();
      r = sys_fcntl(fd, cmd, arg);
      break;
    }
    case 54: {  // ioctl
      const int fd = a.i();
      const u32 request = a.u(), arg = a.u();
      if (request == 0x402C7413) {  // TIOCGETA: how the C library asks "is this a terminal?"
        struct termios tio;
        if (tcgetattr(fd, &tio) != 0) {
          r = SysRet::err(errno);
        } else if (!mem_.range_ok(arg, 44)) {
          r = SysRet::err(EFAULT);
        } else {
          GuestWriter w(mem_, arg);
          w.u32_((u32)tio.c_iflag);
          w.u32_((u32)tio.c_oflag);
          w.u32_((u32)tio.c_cflag);
          w.u32_((u32)tio.c_lflag);
          for (int i = 0; i < 20; i++) w.u8_(tio.c_cc[i]);
          w.u32_((u32)cfgetispeed(&tio));
          w.u32_((u32)cfgetospeed(&tio));
          r = SysRet::ok();
        }
      } else if (request == 0x40087468) {  // TIOCGWINSZ
        struct winsize ws;
        if (ioctl(fd, TIOCGWINSZ, &ws) != 0) {
          r = SysRet::err(errno);
        } else if (!mem_.range_ok(arg, 8)) {
          r = SysRet::err(EFAULT);
        } else {
          GuestWriter w(mem_, arg);
          w.u16_(ws.ws_row);
          w.u16_(ws.ws_col);
          w.u16_(ws.ws_xpixel);
          w.u16_(ws.ws_ypixel);
          r = SysRet::ok();
        }
      } else if (request == 0x4004667A) {  // FIODTYPE: what kind of device; only disks and tapes say
        r = SysRet::err(ENOTTY);
      } else if (request == 0x8004667E) {  // FIONBIO
        if (!mem_.range_ok(arg, 4)) {
          r = SysRet::err(EFAULT);
        } else {
          int enabled = (int)mem_.r32(arg);
          r = result(ioctl(fd, FIONBIO, &enabled));
        }
      } else if (request == 0xC0206911) {  // SIOCGIFFLAGS
        struct ifreq ifr = {};
        static_assert(sizeof ifr == 32);
        if (!mem_.copy_in(&ifr, arg, sizeof ifr)) {
          r = SysRet::err(EFAULT);
        } else if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
          r = SysRet::err(errno);
        } else if (!mem_.copy_out(arg, &ifr, sizeof ifr)) {
          r = SysRet::err(EFAULT);
        } else {
          mem_.w16(arg + 16, (u16)ifr.ifr_flags);
          r = SysRet::ok();
        }
      } else {
        log_warn("unimplemented ioctl 0x%08x", request);
        r = SysRet::err(ENOTTY);
      }
      break;
    }

    // ---- file names ----
    case 33: {  // access
      const std::string path = path_arg();
      const int mode = a.i();
      r = bad ? SysRet::err(EFAULT) : result(access(host_path(path).c_str(), mode));
      break;
    }
    case 284: {  // access_extended; the last argument, a user to answer for, is ignored
      const u32 entries = a.u(), size = a.u(), results = a.u();
      r = sys_access_extended(entries, size, results);
      break;
    }
    case 58: {  // readlink
      const std::string path = path_arg();
      const u32 buf = a.u(), size = a.u();
      if (bad || !mem_.range_ok(buf, size)) {
        r = SysRet::err(EFAULT);
      } else {
        r = result(readlink(host_path(path, false).c_str(), (char*)mem_.host(buf), size));
      }
      break;
    }
    case 12: {  // chdir
      const std::string path = path_arg();
      r = bad ? SysRet::err(EFAULT) : result(chdir(host_path(path).c_str()));
      break;
    }
    case 13:  // fchdir
      r = result(fchdir(a.i()));
      break;
    case 196:
    case 344: {  // getdirentries, getdirentries64
      const int fd = a.i();
      const u32 buf = a.u(), nbytes = a.u(), basep = a.u();
      r = sys_getdirentries(fd, buf, nbytes, basep, num == 344);
      break;
    }
    case 157:
    case 345: {  // statfs, statfs64
      const std::string path = path_arg();
      const u32 buf = a.u();
      struct statfs fs;
      const bool guest_root = path == "/" && !opt_.sysroot.empty();
      if (bad) {
        r = SysRet::err(EFAULT);
      } else if (statfs(guest_root ? opt_.sysroot.c_str() : host_path(path).c_str(), &fs) != 0) {
        r = SysRet::err(errno);
      } else {
        if (guest_root) strlcpy(fs.f_mntonname, "/", sizeof fs.f_mntonname);
        r = sys_statfs(fs, buf, num == 345);
      }
      break;
    }
    case 158:
    case 346: {  // fstatfs, fstatfs64
      const int fd = a.i();
      const u32 buf = a.u();
      struct statfs fs;
      r = fstatfs(fd, &fs) != 0 ? SysRet::err(errno) : sys_statfs(fs, buf, num == 346);
      break;
    }
    case 188:
    case 190:
    case 338:
    case 340: {  // stat, lstat, stat64, lstat64
      const std::string path = path_arg();
      const u32 buf = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_stat(path, buf, num == 188 || num == 338, num >= 338);
      break;
    }
    case 189:
    case 339: {  // fstat, fstat64
      const int fd = a.i();
      r = sys_fstat(fd, a.u(), num == 339);
      break;
    }
    // The "extended" forms also return the file's access control list. Files
    // here have none, which is reported as a size of zero.
    case 279:
    case 280:
    case 341:
    case 342: {  // stat_extended, lstat_extended, stat64_extended, lstat64_extended
      const std::string path = path_arg();
      const u32 buf = a.u();
      a.u();
      const u32 acl_size = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_stat(path, buf, num == 279 || num == 341, num >= 341);
      if (r.kind == SysRet::Value && acl_size && mem_.range_ok(acl_size, 4)) mem_.w32(acl_size, 0);
      break;
    }
    case 281:
    case 343: {  // fstat_extended, fstat64_extended
      const int fd = a.i();
      const u32 buf = a.u();
      a.u();
      const u32 acl_size = a.u();
      r = sys_fstat(fd, buf, num == 343);
      if (r.kind == SysRet::Value && acl_size && mem_.range_ok(acl_size, 4)) mem_.w32(acl_size, 0);
      break;
    }
    case 220: {  // getattrlist
      const std::string path = path_arg();
      const u32 list = a.u(), buf = a.u(), size = a.u(), options = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_getattrlist(path, list, buf, size, options);
      break;
    }
    case 221: {  // setattrlist
      const std::string path = path_arg();
      const u32 list = a.u(), buf = a.u(), size = a.u(), options = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_setattrlist(path, list, buf, size, options);
      break;
    }
    case 18:
    case 347: {  // getfsstat, getfsstat64
      const u32 buf = a.u(), size = a.u();
      const int flags = a.i();
      struct statfs fs;
      const char* root = opt_.sysroot.empty() ? "/" : opt_.sysroot.c_str();
      if (statfs(root, &fs) != 0) {
        r = SysRet::err(errno);
      } else {
        std::vector<struct statfs> mounts;
        const int error = mounted_volumes(mounts, flags);
        if (error) {
          r = SysRet::err(error);
          break;
        }
        std::vector<struct statfs> visible;
        strlcpy(fs.f_mntonname, "/", sizeof fs.f_mntonname);
        visible.push_back(fs);
        for (const struct statfs& mount : mounts) {
          if (mount.f_fsid.val[0] == fs.f_fsid.val[0] && mount.f_fsid.val[1] == fs.f_fsid.val[1])
            continue;
          visible.push_back(mount);
        }
        if (!buf) {
          r = SysRet::ok((u32)visible.size());
          break;
        }
        const u32 entry_size = num == 347 ? 2168u : 272u;
        const u32 n = std::min<u32>(size / entry_size, (u32)visible.size());
        if (n && !mem_.range_ok(buf, n * entry_size)) {
          r = SysRet::err(EFAULT);
          break;
        }
        r = SysRet::ok(n);
        for (u32 i = 0; i < n; i++) {
          const SysRet written = sys_statfs(visible[i], buf + i * entry_size, num == 347);
          if (written.kind != SysRet::Value) {
            r = written;
            break;
          }
        }
      }
      break;
    }
    case 26:  // ptrace: programs use it to refuse debuggers; nothing to refuse
      r = SysRet::ok();
      break;

    case 93:
    case 407: {  // select(count, read set, write set, error set, timeout)
      const int nfds = a.i();
      const u32 sets[3] = {a.u(), a.u(), a.u()};
      const u32 tv = a.u();
      if (nfds < 0 || nfds > 1024) {
        r = SysRet::err(EINVAL);
        break;
      }
      // A descriptor set is an array of 32-bit words on both sides, in the
      // guest's byte order.
      fd_set host[3];
      const u32 words = ((u32)nfds + 31) / 32;
      for (int k = 0; k < 3; k++) {
        FD_ZERO(&host[k]);
        if (!sets[k]) continue;
        if (!mem_.range_ok(sets[k], words * 4)) {
          bad = true;
          break;
        }
        for (u32 w = 0; w < words; w++) host[k].fds_bits[w] = (s32)mem_.r32(sets[k] + 4 * w);
      }
      struct timeval timeout = {};
      if (tv && !mem_.range_ok(tv, 8)) {
        bad = true;
      } else if (tv) {
        timeout.tv_sec = (s32)mem_.r32(tv);
        timeout.tv_usec = (s32)mem_.r32(tv + 4);
      }
      if (bad) {
        r = SysRet::err(EFAULT);
        break;
      }
      const int n = select(nfds, sets[0] ? &host[0] : nullptr, sets[1] ? &host[1] : nullptr,
                           sets[2] ? &host[2] : nullptr, tv ? &timeout : nullptr);
      if (n < 0) {
        r = SysRet::err(errno);
        break;
      }
      for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        for (u32 w = 0; w < words; w++) mem_.w32(sets[k] + 4 * w, (u32)host[k].fds_bits[w]);
      }
      r = SysRet::ok((u32)n);
      break;
    }

    case 230:
    case 417: {  // poll(descriptors, count, timeout in ms)
      // Each entry is a 32-bit descriptor, then the 16-bit events asked
      // about and those that happened, in the guest's byte order; the event
      // bits are the same as this Mac's.
      const u32 fds = a.u(), nfds = a.u();
      const int ms = a.i();
      if (nfds > 4096 || (nfds && !mem_.range_ok(fds, 8ull * nfds))) {
        r = SysRet::err(nfds > 4096 ? EINVAL : EFAULT);
        break;
      }
      std::vector<struct pollfd> host(nfds);
      for (u32 k = 0; k < nfds; k++) {
        host[k].fd = (s32)mem_.r32(fds + 8 * k);
        host[k].events = (s16)mem_.r16(fds + 8 * k + 4);
        host[k].revents = 0;
      }
      const int n = poll(host.data(), (nfds_t)nfds, ms);
      if (n < 0) {
        r = SysRet::err(errno);
        break;
      }
      for (u32 k = 0; k < nfds; k++) mem_.w16(fds + 8 * k + 6, (u16)host[k].revents);
      r = SysRet::ok((u32)n);
      break;
    }

    // ---- sockets ----
    case 27: case 28: case 29: case 30: case 31: case 32: case 97: case 98: case 104: case 105:
    case 106: case 118: case 133: case 134: case 135: case 401: case 402: case 403: case 404:
    case 409: case 413:
      r = sys_socket_call(num, a);
      break;

    // Extended attributes: none.
    case 240:  // listxattr
    case 241:  // flistxattr
      r = SysRet::ok(0);
      break;
    case 234:  // getxattr
    case 235:  // fgetxattr
      r = SysRet::err(ENOATTR);
      break;

    // ---- identity ----
    case 20: r = SysRet::ok((u32)getpid()); break;
    case 24: r = SysRet::ok(opt_.as_root ? ids_[0].load() : getuid()); break;
    case 25: r = SysRet::ok(opt_.as_root ? ids_[1].load() : geteuid()); break;
    case 39: r = SysRet::ok((u32)getppid()); break;
    case 43: r = SysRet::ok(opt_.as_root ? ids_[3].load() : getegid()); break;
    case 47: r = SysRet::ok(opt_.as_root ? ids_[2].load() : getgid()); break;
    case 327: r = SysRet::ok(0); break;  // issetugid
    case 79: {  // getgroups
      const int max = a.i();
      const u32 out = a.u();
      gid_t groups[64];
      const int n = getgroups(64, groups);
      if (n < 0) {
        r = SysRet::err(errno);
      } else if (max == 0) {
        r = SysRet::ok((u32)n);
      } else if (max < n || !mem_.range_ok(out, (u32)n * 4)) {
        r = SysRet::err(EINVAL);
      } else {
        for (int i = 0; i < n; i++) mem_.w32(out + 4 * (u32)i, groups[i]);
        r = SysRet::ok((u32)n);
      }
      break;
    }
    case 142: {  // gethostuuid: a fixed identity for the pretend Mac
      const u32 out = a.u();
      static const u8 uuid[16] = {0xC0, 0xCA, 0x1E, 0x00, 0x10, 0x05, 0x40, 0x00,
                                  0x80, 0x00, 0x00, 0x0A, 0x95, 0x00, 0x00, 0x01};
      r = mem_.copy_out(out, uuid, 16) ? SysRet::ok() : SysRet::err(EFAULT);
      break;
    }
    case 191: {  // pathconf
      const std::string path = path_arg();
      const int name = a.i();
      errno = 0;
      const long v = bad ? -1 : pathconf(host_path(path).c_str(), name);
      r = bad ? SysRet::err(EFAULT) : (v < 0 && errno ? SysRet::err(errno) : SysRet::ok((u32)v));
      break;
    }
    case 192: {  // fpathconf
      const int fd = a.i(), name = a.i();
      errno = 0;
      const long v = fpathconf(fd, name);
      r = v < 0 && errno ? SysRet::err(errno) : SysRet::ok((u32)v);
      break;
    }

    // ---- memory ----
    case 73: {  // munmap
      const u32 addr = a.u(), len = a.u();
      if (addr & 0xFFF) {
        r = SysRet::err(EINVAL);
        break;
      }
      mem_.unmap(addr, len);
      r = SysRet::ok();
      break;
    }
    case 74: {  // mprotect
      const u32 addr = a.u(), len = a.u(), prot = a.u();
      r = mem_.protect(addr, len, (u8)(prot & 7)) ? SysRet::ok() : SysRet::err(ENOMEM);
      break;
    }
    case 75:  // madvise: advice only
      r = SysRet::ok();
      break;
    case 197: {  // mmap
      const u32 addr = a.u(), len = a.u(), prot = a.u(), flags = a.u();
      const s32 fd = a.i();
      const u64 offset = a.q();
      r = sys_mmap(addr, len, prot, flags, fd, offset);
      break;
    }
    case 294:  // shared_region_check_np: there is no prebuilt library cache
      r = SysRet::err(ENOMEM);
      break;

    // ---- time ----
    case 116: {  // gettimeofday
      // The 10.5 kernel hands the time back in the two result registers and
      // the C library stores it; writing the struct as well covers callers
      // that trap directly.
      const u32 tp = a.u();
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      if (tp && mem_.range_ok(tp, 8)) {
        mem_.w32(tp, (u32)tv.tv_sec);
        mem_.w32(tp + 4, (u32)tv.tv_usec);
      }
      // Seconds go in the first result register, microseconds in the second.
      r = SysRet::ok64(arch_ == Arch::PPC ? ((u64)(u32)tv.tv_sec << 32) | (u32)tv.tv_usec
                                          : ((u64)(u32)tv.tv_usec << 32) | (u32)tv.tv_sec);
      break;
    }

    // ---- limits and system information ----
    case 194: {  // getrlimit
      const u32 which = a.u(), out = a.u();
      struct rlimit lim;
      if (getrlimit((int)which, &lim) != 0) {
        r = SysRet::err(errno);
      } else if (!mem_.range_ok(out, 16)) {
        r = SysRet::err(EFAULT);
      } else {
        if (which == RLIMIT_STACK) lim.rlim_cur = lim.rlim_max = 8u << 20;
        mem_.w64(out, lim.rlim_cur);
        mem_.w64(out + 8, lim.rlim_max);
        r = SysRet::ok();
      }
      break;
    }
    case 202: {  // sysctl
      const u32 name = a.u(), namelen = a.u(), oldp = a.u(), oldlenp = a.u(), newp = a.u(),
                newlen = a.u();
      r = sys_sysctl(name, namelen, oldp, oldlenp, newp, newlen);
      break;
    }

    // ---- signals: accepted and ignored until signal delivery exists ----
    case 46: {  // sigaction(sig, new, old)
      const int sig = a.i();
      const u32 act = a.u(), old = a.u();
      r = sys_sigaction(sig, act, old);
      break;
    }
    case 184: {  // sigreturn(context, style)
      const u32 uctx = a.u(), style = a.u();
      r = sys_sigreturn(t, uctx, style);
      break;
    }
    case 111:
    case 410:  // sigsuspend(mask): the mask itself, not a pointer to it
      r = sys_sigsuspend(t, a.u());
      break;
    case 52: {  // sigpending(set)
      const u32 set = a.u();
      sigset_t pending = 0;
      sigpending(&pending);
      if (!mem_.range_ok(set, 4)) {
        r = SysRet::err(EFAULT);
      } else {
        mem_.w32(set, (u32)pending);
        r = SysRet::ok();
      }
      break;
    }
    case 330: {  // __sigwait(set, signal)
      const u32 set = a.u(), out = a.u();
      r = sys_sigwait(set, out);
      break;
    }
    case 83:
    case 86: {  // setitimer(which, new, old), getitimer(which, current)
      const int which = a.i();
      const u32 first = a.u();
      r = num == 83 ? sys_setitimer(which, first, a.u(), true) : sys_setitimer(which, 0, first, false);
      break;
    }
    case 328: {  // __pthread_kill(thread port, signal)
      const u32 port = a.u();
      const int sig = a.i();
      Thread* target;
      {
        std::lock_guard<std::mutex> lk(kmu_);
        target = thread_for_port(port);
      }
      if (!target || target->done) {
        r = SysRet::err(ESRCH);
      } else {
        const int rc = sig ? pthread_kill(target->host, sig) : 0;
        r = rc ? SysRet::err(rc) : SysRet::ok();
      }
      break;
    }
    case 48:
    case 329: {  // sigprocmask(how, set, old), and the per-thread __pthread_sigmask
      const int how = a.i();
      const u32 set = a.u(), old = a.u();
      r = sys_sigprocmask(how, set, old);
      break;
    }
    case 53: {  // sigaltstack(new, old)
      const u32 ss = a.u(), old = a.u();
      r = sys_sigaltstack(t, ss, old);
      break;
    }
    case 37: {  // kill(pid, sig)
      const int pid = a.i(), sig = a.i();
      if (pid == getpid() && sig > 0 && sig < 32) {
        const u32 handler = sigactions_[sig].handler;
        const bool crash = sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                           sig == SIGFPE || sig == SIGTRAP || sig == SIGQUIT || sig == SIGSYS ||
                           sig == SIGEMT;
        if (handler != 0) {
          r = result(kill(pid, sig));  // ignored, or its handler runs on the way out of this call
        } else if (crash) {
          // The usual reason is abort(): the program has given up. Ending
          // with a plain exit keeps this Mac from writing a crash report
          // about cocaine every time a guest does that.
          std::fprintf(stderr, "cocaine: the guest sent itself signal %d (%s)\n", sig, strsignal(sig));
          exit_code_ = 128 + sig;
          exited_ = true;
          r = SysRet::none();
        } else {
          // Left at its default, the host signal does what the guest expects:
          // ends the process, stops it, or nothing at all.
          r = result(kill(pid, sig));
        }
      } else {
        // Another guest process is another host process, and the signal
        // numbers are the same.
        r = result(kill(pid, sig));
      }
      break;
    }

    // ---- more files ----
    case 9: {  // link
      const std::string from = path_arg(), to = path_arg();
      r = bad ? SysRet::err(EFAULT)
              : result(link(host_path(from, false).c_str(), host_path(to, false).c_str()));
      break;
    }
    case 226:  // delete: unlink, with the old Mac OS rules about files in use
    case 10: {  // unlink
      const std::string path = path_arg();
      r = bad ? SysRet::err(EFAULT) : result(unlink(host_path(path, false).c_str()));
      break;
    }
    case 15: {  // chmod
      const std::string path = path_arg();
      const u32 mode = a.u();
      r = bad ? SysRet::err(EFAULT) : result(chmod(host_path(path).c_str(), (mode_t)mode));
      break;
    }
    case 16:
    case 364: {  // chown, lchown
      const std::string path = path_arg();
      const u32 uid = a.u(), gid = a.u();
      if (bad) {
        r = SysRet::err(EFAULT);
      } else if (num == 16) {
        r = result(chown(host_path(path).c_str(), uid, gid));
      } else {
        r = result(lchown(host_path(path, false).c_str(), uid, gid));
      }
      // A daemon told it is root expects this to work. The file stays ours.
      if (opt_.as_root && r.kind == SysRet::Error && r.value == EPERM) r = SysRet::ok();
      break;
    }
    case 123: {  // fchown
      const int fd = a.i();
      const u32 uid = a.u(), gid = a.u();
      r = result(fchown(fd, uid, gid));
      if (opt_.as_root && r.kind == SysRet::Error && r.value == EPERM) r = SysRet::ok();
      break;
    }
    case 124: {  // fchmod
      const int fd = a.i();
      r = result(fchmod(fd, (mode_t)a.u()));
      break;
    }
    case 34: {  // chflags
      const std::string path = path_arg();
      const u32 flags = a.u();
      r = bad ? SysRet::err(EFAULT) : result(chflags(host_path(path).c_str(), flags));
      break;
    }
    case 35: {  // fchflags
      const int fd = a.i();
      r = result(fchflags(fd, a.u()));
      break;
    }
    case 57: {  // symlink(target, link): the target is kept exactly as written
      const std::string target = path_arg(), from = path_arg();
      r = bad ? SysRet::err(EFAULT) : result(symlink(target.c_str(), host_path(from, false).c_str()));
      break;
    }
    case 128: {  // rename
      const std::string from = path_arg(), to = path_arg();
      r = bad ? SysRet::err(EFAULT)
              : result(rename(host_path(from, false).c_str(), host_path(to, false).c_str()));
      break;
    }
    case 132: {  // mkfifo
      const std::string path = path_arg();
      const u32 mode = a.u();
      r = bad ? SysRet::err(EFAULT) : result(mkfifo(host_path(path, false).c_str(), (mode_t)mode));
      break;
    }
    case 136: {  // mkdir
      const std::string path = path_arg();
      const u32 mode = a.u();
      r = bad ? SysRet::err(EFAULT) : result(mkdir(host_path(path, false).c_str(), (mode_t)mode));
      break;
    }
    case 137: {  // rmdir
      const std::string path = path_arg();
      r = bad ? SysRet::err(EFAULT) : result(rmdir(host_path(path, false).c_str()));
      break;
    }
    case 138:
    case 139: {  // utimes(path, times), futimes(fd, times)
      std::string path;
      int fd = -1;
      if (num == 138) {
        path = path_arg();
      } else {
        fd = a.i();
      }
      const u32 times = a.u();
      struct timeval tv[2];
      if (bad || (times && !mem_.range_ok(times, 16))) {
        r = SysRet::err(EFAULT);
        break;
      }
      for (int k = 0; times && k < 2; k++) {
        tv[k].tv_sec = (s32)mem_.r32(times + 8 * k);
        tv[k].tv_usec = (s32)mem_.r32(times + 8 * k + 4);
      }
      r = result(num == 138 ? utimes(host_path(path).c_str(), times ? tv : nullptr)
                            : futimes(fd, times ? tv : nullptr));
      break;
    }
    case 200: {  // truncate
      const std::string path = path_arg();
      const u64 length = a.q();
      r = bad ? SysRet::err(EFAULT) : result(truncate(host_path(path).c_str(), (off_t)length));
      break;
    }
    case 201: {  // ftruncate
      const int fd = a.i();
      r = result(ftruncate(fd, (off_t)a.q()));
      break;
    }
    case 95: r = result(fsync(a.i())); break;
    case 131: {  // flock
      const int fd = a.i();
      r = result(flock(fd, a.i()));
      break;
    }
    case 362:  // kqueue
      r = result(kqueue());
      break;
    case 363: {  // kevent(queue, changes, count, events, room, timeout)
      // What a queue watches (files, processes, signals, Mach ports) is all
      // the host's own here, so the host's queues do the work. Only the
      // record layout differs: the guest's is 20 bytes, with 32-bit fields.
      const int kq = a.i();
      const u32 changes = a.u(), nchanges = a.u(), events = a.u(), nevents = a.u(), timeout = a.u();
      if (nchanges > 4096 || nevents > 4096 ||
          (nchanges && !mem_.range_ok(changes, 20ull * nchanges)) ||
          (nevents && !mem_.range_ok(events, 20ull * nevents)) ||
          (timeout && !mem_.range_ok(timeout, 8))) {
        r = SysRet::err(EFAULT);
        break;
      }
      std::vector<struct kevent> in(nchanges), out(nevents);
      for (u32 k = 0; k < nchanges; k++) {
        const u32 e = changes + 20 * k;
        EV_SET(&in[k], mem_.r32(e), (s16)mem_.r16(e + 4), mem_.r16(e + 6), mem_.r32(e + 8),
               (intptr_t)(s32)mem_.r32(e + 12), (void*)(uintptr_t)mem_.r32(e + 16));
      }
      struct timespec ts = {};
      if (timeout) {
        ts.tv_sec = (s32)mem_.r32(timeout);
        ts.tv_nsec = (s32)mem_.r32(timeout + 4);
      }
      const int n = kevent(kq, in.data(), (int)nchanges, out.data(), (int)nevents,
                           timeout ? &ts : nullptr);
      if (n < 0) {
        r = SysRet::err(errno);
        break;
      }
      for (int k = 0; k < n; k++) {
        const u32 e = events + 20 * (u32)k;
        mem_.w32(e, (u32)out[k].ident);
        mem_.w16(e + 4, (u16)out[k].filter);
        // A change that failed comes back flagged EV_ERROR alone, as Leopard
        // had it; this Mac keeps the change's own flags too, and libevent
        // (Transmission's) takes any other answer to its test of a bad
        // descriptor for a broken kqueue and falls back to poll.
        mem_.w16(e + 6, (out[k].flags & EV_ERROR) ? (u16)EV_ERROR : out[k].flags);
        mem_.w32(e + 8, out[k].fflags);
        mem_.w32(e + 12, (u32)out[k].data);
        mem_.w32(e + 16, (u32)(uintptr_t)out[k].udata);
      }
      r = SysRet::ok((u32)n);
      break;
    }
    case 266:
    case 267: {  // shm_open(name, flags, mode), shm_unlink(name)
      const std::string name = path_arg();
      const int oflag = a.i();
      const u32 mode = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_shm(num, name, oflag, mode);
      break;
    }
    case 36:   // sync
    case 65:   // msync: file mappings are private copies for now
    case 203:  // mlock
    case 204:  // munlock
      r = SysRet::ok();
      break;
    case 89: r = SysRet::ok((u32)getdtablesize()); break;

    // ---- who the process is ----
    case 49: {  // getlogin(buffer, size)
      const u32 buf = a.u(), size = a.u();
      const char* name = getlogin();
      if (!name) name = "";
      const size_t n = strlen(name) + 1;
      if (n > size || !mem_.range_ok(buf, (u32)n)) {
        r = SysRet::err(ERANGE);
      } else {
        memcpy(mem_.host(buf), name, n);
        r = SysRet::ok();
      }
      break;
    }
    case 50:  // setlogin: the session's login name, which stays what it is
      r = SysRet::ok();
      break;
    case 23:
    case 181:
    case 182:
    case 183: {  // setuid, setgid, setegid, seteuid
      const u32 id = a.u();
      if (opt_.as_root) {
        // Root may become anyone and, having kept its real identity, come back.
        if (num == 23) ids_[0] = ids_[1] = id;
        if (num == 181) ids_[2] = ids_[3] = id;
        if (num == 182) ids_[3] = id;
        if (num == 183) ids_[1] = id;
        r = SysRet::ok();
      } else {
        r = result(num == 23 ? setuid(id) : num == 181 ? setgid(id) : num == 182 ? setegid(id) : seteuid(id));
      }
      break;
    }
    case 126:
    case 127: {  // setreuid, setregid (real, effective); -1 leaves one alone
      const u32 real = a.u(), effective = a.u();
      if (opt_.as_root) {
        const int base = num == 126 ? 0 : 2;
        if (real != 0xFFFFFFFFu) ids_[base] = real;
        if (effective != 0xFFFFFFFFu) ids_[base + 1] = effective;
        r = SysRet::ok();
      } else {
        r = result(num == 126 ? setreuid(real, effective) : setregid(real, effective));
      }
      break;
    }
    case 151: r = result(getpgid(a.i())); break;
    case 310: r = result(getsid(a.i())); break;
    case 96:   // setpriority
    case 100:  // getpriority
    case 195:  // setrlimit
      r = SysRet::ok();
      break;

    // ---- processes ----
    case 2:
    case 66:  // fork, vfork
      r = sys_fork(t);
      break;
    case 59: {  // execve
      const std::string path = path_arg();
      const u32 argv = a.u(), envp = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_execve(path, argv, envp);
      break;
    }
    case 244: {  // posix_spawn(pid, path, description, argv, envp)
      const u32 pid_out = a.u();
      const std::string path = path_arg();
      const u32 desc = a.u(), argv = a.u(), envp = a.u();
      r = bad ? SysRet::err(EFAULT) : sys_posix_spawn(pid_out, path, desc, argv, envp);
      break;
    }
    case 7:
    case 400: {  // wait4
      const int pid = a.i();
      const u32 status = a.u();
      const int options = a.i();
      r = sys_wait4(pid, status, options, a.u());
      break;
    }
    case 42: {  // pipe: the two descriptors come back in the two result registers
      int fds[2];
      r = pipe(fds) != 0 ? SysRet::err(errno) : two_results((u32)fds[0], (u32)fds[1]);
      break;
    }
    case 81: r = SysRet::ok((u32)getpgrp()); break;
    case 82: {  // setpgid
      const int pid = a.i(), pgid = a.i();
      r = result(setpgid(pid, pgid));
      break;
    }
    case 147: r = result(setsid()); break;
    case 60: r = SysRet::ok(umask((mode_t)a.u())); break;

    // ---- threads ----
    case 366:  // bsdthread_register(thread start, work-queue start, pthread size)
      thread_start_ = a.u();
      a.u();
      pthread_size_ = a.u();
      r = SysRet::ok();
      break;
    case 360: {  // bsdthread_create(function, argument, stack, pthread, flags)
      const u32 func = a.u(), arg = a.u(), stack = a.u(), pthread = a.u(), flags = a.u();
      r = sys_bsdthread_create(t, func, arg, stack, pthread, flags);
      break;
    }
    case 361: {  // bsdthread_terminate(stack, size, thread port, semaphore to signal)
      const u32 stack = a.u(), size = a.u();
      a.u();
      const u32 sem = a.u();
      if (size) mem_.unmap(stack, size);
      if (sem) ::semaphore_signal(sem);
      t.done = true;
      r = SysRet::none();
      break;
    }
    case 334: {  // __semwait_signal(wait on, signal, timed, relative, seconds, nanoseconds)
      const u32 wait_on = a.u(), signal = a.u(), timed = a.u(), relative = a.u();
      const u64 sec = a.u();
      const u32 nsec = a.u();
      u64 ns = sec * 1000000000ull + nsec;
      if (timed && !relative) {  // an absolute time of day
        struct timeval now;
        gettimeofday(&now, nullptr);
        const u64 now_ns = (u64)now.tv_sec * 1000000000ull + (u64)now.tv_usec * 1000;
        ns = ns > now_ns ? ns - now_ns : 0;
      }
      // Give up one semaphore (a mutex's) and wait on another (a condition's)
      // in one step, which the host's semaphores do natively.
      kern_return_t kr;
      if (timed) {
        const mach_timespec_t ts = {(unsigned)(ns / 1000000000ull), (clock_res_t)(ns % 1000000000ull)};
        kr = signal ? ::semaphore_timedwait_signal(wait_on, signal, ts)
                    : ::semaphore_timedwait(wait_on, ts);
      } else {
        kr = signal ? ::semaphore_wait_signal(wait_on, signal) : ::semaphore_wait(wait_on);
      }
      r = kr == KERN_SUCCESS ? SysRet::ok()
                             : SysRet::err(kr == KERN_OPERATION_TIMED_OUT ? ETIMEDOUT
                                           : kr == KERN_ABORTED           ? EINTR
                                                                          : EINVAL);
      break;
    }
    case 331:  // __disable_threadsignal
      r = SysRet::ok();
      break;
    case 333:  // __pthread_canceled: nobody cancels threads from outside yet
      r = SysRet::err(EINVAL);
      break;
    case 167:  // mount
    case 159:  // unmount
      r = SysRet::err(EPERM);
      break;
    case 117: {  // getrusage(who, usage)
      const int who = a.i();
      const u32 out = a.u();
      struct rusage ru;
      if (getrusage(who, &ru) != 0) {
        r = SysRet::err(errno);
      } else if (!mem_.range_ok(out, 72)) {
        r = SysRet::err(EFAULT);
      } else {
        // Two times of two 32-bit words each, then fourteen 32-bit counters.
        const long counters[14] = {ru.ru_maxrss / 1024, ru.ru_ixrss, ru.ru_idrss, ru.ru_isrss,
                                   ru.ru_minflt, ru.ru_majflt, ru.ru_nswap, ru.ru_inblock,
                                   ru.ru_oublock, ru.ru_msgsnd, ru.ru_msgrcv, ru.ru_nsignals,
                                   ru.ru_nvcsw, ru.ru_nivcsw};
        mem_.w32(out, (u32)ru.ru_utime.tv_sec);
        mem_.w32(out + 4, (u32)ru.ru_utime.tv_usec);
        mem_.w32(out + 8, (u32)ru.ru_stime.tv_sec);
        mem_.w32(out + 12, (u32)ru.ru_stime.tv_usec);
        for (u32 k = 0; k < 14; k++) mem_.w32(out + 16 + 4 * k, (u32)counters[k]);
        r = SysRet::ok();
      }
      break;
    }

    case 293: {  // identitysvc: where the kernel hands the directory daemon its questions
      // (which user has this identifier, is this user in that group). The
      // daemon registers, then parks a thread here waiting for work. This
      // kernel never has any, so the thread waits until a signal comes.
      const int opcode = a.i();
      if (opcode == 0) {
        r = SysRet::ok();
      } else {
        pause();
        r = SysRet::err(EINTR);
      }
      break;
    }
    case 350:  // audit
    case 351:  // auditon
    case 353:  // getauid
    case 355:  // getaudit
    case 357:  // getaudit_addr
      r = SysRet::err(ENOSYS);  // security auditing is not built into this kernel
      break;
    case 380:  // __mac_execve
    case 381:  // __mac_syscall: asks a security policy (file quarantine, the sandbox) a question
      r = SysRet::err(103);  // ENOPOLICY: no such policy loaded
      break;

    default:
      if (warned_bsd_.insert(num).second) {
        log_warn("unimplemented system call %u (%s)", num, bsd_name(num));
      }
      break;
  }

  if (opt_.strace) {
    switch (r.kind) {
      case SysRet::Value: std::fprintf(stderr, " = 0x%x\n", (u32)r.value); break;
      case SysRet::Value64: std::fprintf(stderr, " = 0x%llx\n", (unsigned long long)r.value); break;
      case SysRet::Error: std::fprintf(stderr, " = error %d (%s)\n", (int)r.value, strerror((int)r.value)); break;
      case SysRet::NoReturn: std::fprintf(stderr, "\n"); break;
    }
  }
  return r;
}
