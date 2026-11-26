#include <dlfcn.h>
#include <execinfo.h>
#include <thread>
#include <climits>
#include "cpu/ppc/ppc_jit.h"
#include "kernel/process.h"
#include "kernel/protect.h"

#include <signal.h>
#include <unistd.h>

#include <ctime>

namespace {

// Mac OS X gives 32-bit programs a stack that ends at 0xC0000000.
constexpr u32 kStackTop = 0xC0000000u;
constexpr u32 kStackSize = 8u << 20;

Process* g_process = nullptr;
thread_local Thread* t_current = nullptr;  // the guest thread this host thread runs

// Interpreters run in slices this long, so a thread notices promptly when the
// process has exited or it has been told to end.
constexpr u64 kSlice = 1 << 20;

void on_host_fault(int sig, siginfo_t* info, void* context) {
  if (g_process) g_process->report_fault(sig, info->si_addr, context);
  _exit(128 + sig);
}

u8 prot_bits(u32 vmprot) {
  u8 p = 0;
  if (vmprot & 1) p |= GuestMemory::kRead;
  if (vmprot & 2) p |= GuestMemory::kWrite;
  if (vmprot & 4) p |= GuestMemory::kExec;
  return p;
}

}  // namespace

Process::Process(Arch arch, GuestMemory& mem, const Options& opt)
    : arch_(arch), mem_(mem), opt_(opt) {
  // The kernel objects every process can name from the start.
  setup_ports(main_);
  // Signals that whoever started this process had ignored stay ignored, and
  // the guest can find that out, as a shell does before running a job.
  for (int sig = 1; sig < 32; sig++) {
    struct sigaction cur;
    if (sigaction(sig, nullptr, &cur) == 0 && cur.sa_handler == SIG_IGN) sigactions_[sig].handler = 1;
  }
}

bool Process::map_image(const MachImage& img, std::string& err) {
  for (const MachSegment& seg : img.segments) {
    if (!seg.vmsize) continue;
    // __PAGEZERO and friends reserve addresses without backing them.
    if (seg.initprot == 0 && seg.filesize == 0) continue;
    if (mem_.any_mapped(seg.vmaddr, seg.vmsize)) {
      err = img.path + ": segment " + seg.name + " overlaps memory that is already in use";
      return false;
    }
    if (!mem_.map(seg.vmaddr, seg.vmsize, prot_bits(seg.initprot))) {
      err = img.path + ": cannot map segment " + seg.name;
      return false;
    }
    if (seg.filesize) memcpy(mem_.host(seg.vmaddr), img.data.data() + seg.fileoff, seg.filesize);
    if (seg.filesize && (seg.flags & kSegmentProtected)) {
      // One of the few programs Apple stored encrypted: what the kernel
      // would decrypt page by page as it was read in is decrypted here.
      std::string why;
      if (!unprotect_segment(mem_.host(seg.vmaddr), seg.fileoff, seg.filesize, opt_.sysroot, why)) {
        err = img.path + ": " + why;
        return false;
      }
    }
  }
  return true;
}

// Builds the stack a new Mac OS X process starts with. From the stack pointer
// upward: [Mach-O header address, only for dyld] argc, argv[], 0, envp[], 0,
// the executable's path pointer, 0, and then the strings themselves.
bool Process::setup_stack(const std::vector<std::string>& argv,
                          const std::vector<std::string>& envp, bool push_header,
                          u32 header_addr, u32& sp, std::string& err) {
  if (!mem_.map(kStackTop - kStackSize, kStackSize, GuestMemory::kRead | GuestMemory::kWrite)) {
    err = "cannot map the guest stack";
    return false;
  }

  size_t strings = 0;
  for (const std::string& s : argv) strings += s.size() + 1;
  for (const std::string& s : envp) strings += s.size() + 1;
  strings += exe_path_.size() + 1;
  const size_t words = (push_header ? 1 : 0) + 1 + argv.size() + 1 + envp.size() + 1 + 2;
  if (strings + words * 4 + 4096 > kStackSize / 2) {
    err = "arguments and environment are too large for the guest stack";
    return false;
  }

  u32 p = kStackTop - 4;  // leave a zero word at the very top
  auto push_string = [&](const std::string& s) {
    p -= (u32)s.size() + 1;
    memcpy(mem_.host(p), s.c_str(), s.size() + 1);
    return p;
  };
  const u32 exec_path = push_string(exe_path_);
  std::vector<u32> arg_ptrs, env_ptrs;
  for (const std::string& s : argv) arg_ptrs.push_back(push_string(s));
  for (const std::string& s : envp) env_ptrs.push_back(push_string(s));

  p = (p - (u32)words * 4) & ~15u;
  sp = p;
  auto push_word = [&](u32 v) {
    mem_.w32(p, v);
    p += 4;
  };
  if (push_header) push_word(header_addr);
  push_word((u32)argv.size());
  for (u32 a : arg_ptrs) push_word(a);
  push_word(0);
  for (u32 a : env_ptrs) push_word(a);
  push_word(0);
  push_word(exec_path);
  push_word(0);
  return true;
}

bool Process::load(const MachImage& exe, const std::vector<std::string>& host_argv,
                   const std::vector<std::string>& envp, std::string& err) {
  if (exe.filetype != kMachExecute) {
    err = exe.path + ": not a program (Mach-O file type " + std::to_string(exe.filetype) + ")";
    return false;
  }
  // A program inside the system root sees itself at its Mac OS X path.
  std::vector<std::string> argv = host_argv;
  exe_path_ = guest_path(argv[0]);
  const size_t slash = exe_path_.rfind('/');
  proc_name_ = exe_path_.substr(slash == std::string::npos ? 0 : slash + 1, 32);
  {
    static const char kLoginWindow[] = "/loginwindow";
    const size_t n = sizeof kLoginWindow - 1;
    is_login_window_ = exe_path_.size() > n && exe_path_.compare(exe_path_.size() - n, n, kLoginWindow) == 0;
  }
  // Whether this is a program that puts windows up, which a session wants to
  // know from the start (see ensure_session_server): one in an application
  // bundle, or one that links the libraries windows are made with.
  // (Not the helper applications inside frameworks, such as the one Python
  // runs every script in: most of what those run never opens a window, and
  // if it does the window server is started then.)
  has_windows_ = exe_path_.find(".app/Contents/MacOS/") != std::string::npos &&
                 exe_path_.find(".framework/") == std::string::npos;
  for (const std::string& library : exe.dylibs) {
    for (const char* framework : {"/Cocoa.framework/", "/Carbon.framework/", "/AppKit.framework/",
                                  "/ApplicationServices.framework/", "/OpenGL.framework/",
                                  "/GLUT.framework/", "/QuickTime.framework/"}) {
      if (library.find(framework) != std::string::npos) has_windows_ = true;
    }
  }
  // A program started by another guest program is called whatever that
  // program chose to call it ("-sh" for a login shell, say).
  argv[0] = opt_.argv0.empty() ? exe_path_ : opt_.argv0;
  argv_ = argv;
  envp_ = envp;

  if (!map_image(exe, err)) return false;
  if (!setup_commpage(err)) return false;

  u32 entry;
  bool via_dyld = false;
  if (!exe.dylinker.empty()) {
    // A dynamically linked program starts in dyld, which then loads the
    // system libraries. Both come from a real Mac OS X system root.
    if (opt_.sysroot.empty()) {
      err = exe.path + " is dynamically linked: it needs " + exe.dylinker +
            " and the Mac OS X system libraries.\n"
            "cocaine loads those from a Mac OS X system root, and none is set. "
            "Pass --sysroot DIR or set COCAINE_SYSROOT (see README.md).";
      return false;
    }
    const std::string dyld_path = opt_.sysroot + exe.dylinker;
    MachImage dyld;
    if (!load_macho(dyld_path, arch_, dyld, err)) return false;
    if (dyld.filetype != kMachDylinker || !dyld.has_entry) {
      err = dyld_path + ": not a dynamic linker";
      return false;
    }
    if (!map_image(dyld, err)) return false;
    entry = dyld.entry;
    via_dyld = true;
  } else if (exe.has_entry) {
    entry = exe.entry;
  } else {
    err = exe.path + ": has no entry point";
    return false;
  }

  u32 sp;
  if (!setup_stack(argv, envp, via_dyld, exe.header_addr(), sp, err)) return false;

  if (arch_ == Arch::PPC) {
    PPCState& s = main_.ppc;
    s.mem = mem_.base();
    s.trace = opt_.trace;
    s.trace_registers = opt_.trace_registers;
    s.pc = entry;
    s.gpr[1] = sp;
  } else {
    X86State& s = main_.x86;
    s.mem = mem_.base();
    s.trace = opt_.trace;
    s.trace_registers = opt_.trace_registers;
    s.eip = entry;
    s.reg[X86State::ESP] = sp;
    // The flat user segments Mac OS X hands every 32-bit process.
    s.seg[X86State::CS] = 0x17;
    s.seg[X86State::DS] = s.seg[X86State::ES] = s.seg[X86State::SS] = 0x1F;
  }
  log_info("loaded %s (%s), entry 0x%08x, stack 0x%08x%s", exe.path.c_str(), arch_name(arch_),
           entry, sp, via_dyld ? ", starting in dyld" : "");
  return true;
}

const char* Process::image_of(u32 pc, u32& offset) const {
  // try_lock: a crash report must not wait on a lock the crashed thread may hold.
  if (!images_mu_.try_lock()) return nullptr;
  const char* found = nullptr;
  for (const MappedImage& img : images_) {
    if (pc >= img.at && pc - img.at < img.len) {
      offset = pc - img.at;
      found = img.path.c_str();
      break;
    }
  }
  images_mu_.unlock();
  return found;
}

void Process::dump_registers(const Thread& t) const {
  if (arch_ == Arch::PPC) {
    const PPCState& s = t.ppc;
    for (int i = 0; i < 32; i += 4) {
      std::fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, s.gpr[i], i + 1,
                   s.gpr[i + 1], i + 2, s.gpr[i + 2], i + 3, s.gpr[i + 3]);
    }
    std::fprintf(stderr, "  pc  %08x  lr  %08x  ctr %08x  cr  %08x  xer %08x\n", s.pc, s.lr, s.ctr,
                 s.cr, s.xer);
    // The calls that led here: each stack frame starts with a pointer to
    // the one before, and the return address sits two words into that one.
    auto word = [&](u32 at, u32& v) {
      if (!guest_readable(at) || !guest_readable(at + 3)) return false;
      const u8* b = mem_.base() + at;
      v = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
      return true;
    };
    std::fprintf(stderr, "  called from:");
    u32 frame = s.gpr[1];
    for (int depth = 0; depth < 16; depth++) {
      u32 back = 0, ret = 0;
      if (!word(frame, back) || back <= frame || !word(back + 8, ret)) break;
      u32 off = 0;
      const char* lib = image_of(ret, off);
      if (const char* slash = lib ? strrchr(lib, '/') : nullptr) lib = slash + 1;
      std::fprintf(stderr, "\n    %08x", ret);
      if (lib) std::fprintf(stderr, "  %s + 0x%x", lib, off);
      frame = back;
    }
    std::fprintf(stderr, "\n");
    // COCAINE_FAULT_MEM=r6:160,@2eebd0:16: memory at those registers or addresses, for working on cocaine.
    if (const char* spec = getenv("COCAINE_FAULT_MEM")) {
      for (const char* q = spec; *q;) {
        int r = 0, bytes = 64;
        u32 fixed = 0;
        const bool absolute = std::sscanf(q, "@%x:%d", &fixed, &bytes) >= 1;  // @address:bytes
        if (absolute || (std::sscanf(q, "r%d:%d", &r, &bytes) >= 1 && r >= 0 && r < 32)) {
          const u32 at = (absolute ? fixed : s.gpr[r]) & ~3u;
          std::fprintf(stderr, "  %s%d -> %08x:", absolute ? "@" : "r", absolute ? 0 : r, at);
          for (int k = 0; k < bytes; k += 4) {
            u32 v = 0;
            if (k % 32 == 0) std::fprintf(stderr, "\n    +%03x", k);
            if (word(at + (u32)k, v)) {
              std::fprintf(stderr, " %08x", v);
            } else {
              std::fprintf(stderr, " ????????");
            }
          }
          std::fprintf(stderr, "\n");
        }
        while (*q && *q != ',') q++;
        if (*q) q++;
      }
    }
  } else {
    const X86State& s = t.x86;
    std::fprintf(stderr, "  eax %08x  ecx %08x  edx %08x  ebx %08x\n", s.reg[0], s.reg[1], s.reg[2],
                 s.reg[3]);
    if (getenv("COCAINE_FAULT_CODE")) {  // for working on cocaine: the code around the instruction
      std::fprintf(stderr, "  code from %08x:", s.eip - 256);
      for (u32 a = s.eip - 256; a < s.eip + 32; a++) {
        if (guest_readable(a)) std::fprintf(stderr, "%s%02x", (a - s.eip) % 32 == 0 ? "\n    " : " ", mem_.r8(a));
      }
      std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "  esp %08x  ebp %08x  esi %08x  edi %08x\n", s.reg[4], s.reg[5], s.reg[6],
                 s.reg[7]);
    std::fprintf(stderr, "  eip %08x  eflags %08x\n", s.eip, s.eflags);
  }
}

void Process::report_fault(int sig, const void* host_addr, const void* context) {
  Thread& cur = t_current ? *t_current : main_;
  // Translated code keeps some of the guest's registers in the host's, and
  // not its pc at all: the translator fills those in from where it stopped.
  if (arch_ == Arch::PPC) {
    ppc_jit_fault(context, cur.ppc);
  } else {
    x86_jit_fault(context, cur.x86);
  }
  const auto* uc = (const ucontext_t*)context;
  const void* host_pc =
      uc && uc->uc_mcontext ? (const void*)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss) : nullptr;
  const u32 pc = arch_ == Arch::PPC ? cur.ppc.pc : cur.x86.eip;
  // Which library the instruction is in, if a mapped one.
  if (images_mu_.try_lock()) {
    for (const MappedImage& img : images_) {
      if (pc >= img.at && pc - img.at < img.len) {
        std::fprintf(stderr, "cocaine: that instruction is 0x%x into the code of %s\n", pc - img.at, img.path.c_str());
      }
    }
    images_mu_.unlock();
  }
  if (mem_.contains_host(host_addr)) {
    const u64 guest = (u64)((const u8*)host_addr - mem_.base());
    std::fprintf(stderr,
                 "cocaine: the guest touched unmapped memory at 0x%08llx (instruction at 0x%08x)\n",
                 (unsigned long long)guest, pc);
    if (getenv("COCAINE_FAULT_CODE")) {  // for working on cocaine: which host code made the access
      Dl_info info = {};
      if (host_pc && dladdr(host_pc, &info) && info.dli_fbase) {
        std::fprintf(stderr, "  from host code at %p: %s +0x%lx (in %s, loaded at %p)\n", host_pc,
                     info.dli_sname ? info.dli_sname : "?",
                     info.dli_saddr ? (unsigned long)((const u8*)host_pc - (const u8*)info.dli_saddr) : 0ul,
                     info.dli_fname, info.dli_fbase);
      } else {
        std::fprintf(stderr, "  from host code at %p (translated code)\n", host_pc);
      }
      void* frames[24];
      backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
    }
  } else {
    std::fprintf(stderr,
                 "cocaine: internal error: signal %d at host address %p, from host code at %p "
                 "(guest instruction at 0x%08x)\n",
                 sig, host_addr, host_pc, pc);
    // cocaine's own bug, so say where in cocaine.
    void* frames[40];
    backtrace_symbols_fd(frames, backtrace(frames, 40), 2);
  }
  dump_registers(cur);
}

int Process::run() {
  g_process = this;
  // Translations of PowerPC code have to be dropped when the memory they
  // were made from is unmapped or mapped over.
  GuestMemory::on_change = arch_ == Arch::PPC ? ppc_jit_invalidate : x86_jit_invalidate;
  // Intel code changes itself without notice; the translator asks which
  // code could.
  x86_jit_writable = [](u32 addr) { return g_process && g_process->guest_writable(addr); };
  x86_jit_readable = [](u32 addr) { return g_process && g_process->guest_readable(addr); };
  struct sigaction sa = {};
  sa.sa_sigaction = on_host_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  // For working on cocaine itself: COCAINE_DUMP=address:size:seconds:file
  // writes that much guest memory to file.<pid> after that long, if the
  // process has it mapped by then. (How the window server's display was
  // first looked at.)
  if (const char* dump = getenv("COCAINE_DUMP")) {
    unsigned long addr = 0, size = 0, seconds = 0;
    char path[PATH_MAX] = "";
    if (sscanf(dump, "%lx:%lu:%lu:%1000s", &addr, &size, &seconds, path) == 4) {
      const std::string file = std::string(path) + "." + std::to_string(getpid());
      sigset_t everything, before;
      sigfillset(&everything);
      pthread_sigmask(SIG_SETMASK, &everything, &before);
      std::thread([this, addr, size, seconds, file] {
        sleep((unsigned)seconds);
        if (!mem_.range_ok((u32)addr, size)) return;
        if (FILE* f = fopen(file.c_str(), "wb")) {
          fwrite(mem_.host((u32)addr), 1, size, f);
          fclose(f);
        }
      }).detach();
      pthread_sigmask(SIG_SETMASK, &before, nullptr);
    }
  }

  run_started_ = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  int rc = run_thread(main_);
  {
    std::unique_lock<std::mutex> lk(kmu_);
    kcv_.wait(lk, [this] {
      if (exited_) return true;
      for (const auto& t : threads_) {
        if (!t->done) return false;
      }
      return true;
    });
  }
  // If the main thread ended by itself, the process lives on until the last
  // thread does or something calls exit.
  if (exited_) rc = exit_code_;
  report_stats();
  return rc;
}

void Process::report_stats() const {
  if (arch_ == Arch::PPC) {
    ppc_jit_report();
  } else {
    x86_jit_report();
  }
  if (opt_.stats) {
    const double secs = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - run_started_) / 1e9;
    const u64 n = arch_ == Arch::PPC ? main_.ppc.icount : main_.x86.icount;
    std::fprintf(stderr, "cocaine: %llu guest instructions in %.3f s (%.1f million per second)\n",
                 (unsigned long long)n, secs, secs > 0 ? (double)n / secs / 1e6 : 0.0);
  }
}

// region_at gives the next region up when nothing is mapped at the address
// itself, so the region has to start at or below it to count.
bool Process::guest_writable(u32 addr) const {
  u32 start = 0, length = 0;
  u8 prot = 0;
  return mem_.region_at(addr, start, length, prot) && start <= addr && (prot & GuestMemory::kWrite);
}

bool Process::guest_readable(u32 addr) const {
  u32 start = 0, length = 0;
  u8 prot = 0;
  return mem_.region_at(addr, start, length, prot) && start <= addr && (prot & GuestMemory::kRead);
}

int Process::run_thread(Thread& t) {
  t_current = &t;
  t.host = pthread_self();
  t.host_port = pthread_mach_thread_np(t.host);
  return arch_ == Arch::PPC ? run_ppc(t) : run_x86(t);
}

int Process::run_ppc(Thread& t) {
  PPCState& s = t.ppc;
  for (;;) {
    if (exited_) return exit_code_;
    if (t.done) return 0;
    if (t.returned) {  // the end of a guest function cocaine called
      t.returned = false;
      return 0;
    }
    if (signal_arrived()) {
      deliver_signals(t);
      if (exited_) return exit_code_;
    }
    const u64 left = opt_.max_insns > s.icount ? opt_.max_insns - s.icount : 0;
    const PPCStop st = ppc_run(s, left < kSlice ? left : kSlice);
    switch (st.kind) {
      case PPCStopKind::Syscall:
        ppc_syscall(t);
        if (exited_) return exit_code_;
        break;
      case PPCStopKind::HostCall:
        host_call(t, st.insn & 0xFFFF);
        break;
      case PPCStopKind::Trap:
        std::fprintf(stderr, "cocaine: the guest hit a trap instruction at 0x%08x\n", st.pc);
        dump_registers(t);
        return 128 + SIGTRAP;
      case PPCStopKind::Illegal:
        std::fprintf(stderr, "cocaine: PowerPC instruction 0x%08x at 0x%08x is not implemented\n",
                     st.insn, st.pc);
        dump_registers(t);
        return 128 + SIGILL;
      case PPCStopKind::Budget:
        if (left > kSlice) break;  // only the end of a slice
        log_warn("stopped after %llu guest instructions (--max-insns)",
                 (unsigned long long)s.icount);
        return 124;
    }
  }
}

int Process::run_x86(Thread& t) {
  X86State& s = t.x86;
  for (;;) {
    if (exited_) return exit_code_;
    if (t.done) return 0;
    if (t.returned) {  // the end of a guest function cocaine called
      t.returned = false;
      return 0;
    }
    if (signal_arrived()) {
      deliver_signals(t);
      if (exited_) return exit_code_;
    }
    const u64 left = opt_.max_insns > s.icount ? opt_.max_insns - s.icount : 0;
    const X86Stop st = x86_run(s, left < kSlice ? left : kSlice);
    switch (st.kind) {
      case X86StopKind::Int:
        if (st.vector == kX86HostCallVector) {
          // The call's identifier follows the instruction.
          const u32 id = mem_.r16(s.eip);
          s.eip += 2;
          host_call(t, id);
          break;
        }
        x86_syscall(t, false, st.vector);
        if (exited_) return exit_code_;
        break;
      case X86StopKind::Sysenter:
        x86_syscall(t, true, 0);
        if (exited_) return exit_code_;
        break;
      case X86StopKind::Breakpoint:
        std::fprintf(stderr, "cocaine: the guest hit a breakpoint (int3) before 0x%08x\n", st.eip);
        dump_registers(t);
        return 128 + SIGTRAP;
      case X86StopKind::DivideError:
        std::fprintf(stderr, "cocaine: the guest divided by zero at 0x%08x\n", st.eip);
        dump_registers(t);
        return 128 + SIGFPE;
      case X86StopKind::Halt:
        std::fprintf(stderr, "cocaine: the guest ran a privileged hlt at 0x%08x\n", st.eip);
        dump_registers(t);
        return 128 + SIGSEGV;
      case X86StopKind::Illegal:
      case X86StopKind::Unimplemented: {
        const u8* p = mem_.host(st.eip);
        std::fprintf(stderr,
                     "cocaine: Intel instruction at 0x%08x is %s: %s\n"
                     "  bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     st.eip, st.kind == X86StopKind::Illegal ? "invalid" : "not implemented",
                     st.what ? st.what : "?", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        dump_registers(t);
        return 128 + SIGILL;
      }
      case X86StopKind::Budget:
        if (left > kSlice) break;  // only the end of a slice
        log_warn("stopped after %llu guest instructions (--max-insns)",
                 (unsigned long long)s.icount);
        return 124;
    }
  }
}
