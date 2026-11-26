// Guest threads.
//
// Each guest thread is a host thread running its own interpreter over the
// shared guest memory. Mac OS X 10.5 creates threads two ways: Intel's C
// library asks the BSD layer (bsdthread_create), which also allocates the
// stack; PowerPC's still uses the older Mach call that takes a complete
// register state (thread_create_running, in mach.cpp).

#include <chrono>
#include <thread>

#include <mach/mach.h>
#include <unistd.h>

#include "kernel/process.h"

Thread* Process::new_thread() {
  threads_.push_back(std::make_unique<Thread>());
  Thread* t = threads_.back().get();
  t->number = (int)threads_.size();
  t->self_port = new_kport(KObject::Thread, t);
  if (arch_ == Arch::PPC) {
    t->ppc.mem = mem_.base();
    t->ppc.trace = opt_.trace;
    t->ppc.trace_registers = opt_.trace_registers;
  } else {
    X86State& s = t->x86;
    s.mem = mem_.base();
    s.trace = opt_.trace;
    s.trace_registers = opt_.trace_registers;
    s.seg[X86State::CS] = 0x17;
    s.seg[X86State::DS] = s.seg[X86State::ES] = s.seg[X86State::SS] = 0x1F;
  }
  return t;
}

void Process::start_thread(Thread* t) {
  std::thread([this, t] {
    const int rc = run_thread(*t);
    // After a fork from this thread there is no main thread to end the process.
    if (sole_thread_ == t) _exit(exited_ ? exit_code_ : rc);
    // A thread that stops for any reason other than ending normally (a bad
    // instruction, say) takes the process down with it, as a crash would.
    if (!t->done && !exited_) {
      exit_code_ = rc;
      exited_ = true;
      kcv_.notify_all();
    }
    // The thread's port dies with it, as the real one would: anyone still
    // holding a reference is left with a dead name.
    {
      std::lock_guard<std::mutex> lk(kmu_);
      t->done = true;
      if (kports_.erase(t->self_port)) {
        mach_port_mod_refs(mach_task_self(), t->self_port, MACH_PORT_RIGHT_RECEIVE, -1);
      }
    }
    kcv_.notify_all();
  }).detach();
}

Thread* Process::thread_for_port(u32 port) {
  if (main_.self_port == port) return &main_;
  for (const auto& t : threads_) {
    if (t->self_port == port) return t.get();
  }
  return nullptr;
}

// bsdthread_create: start a thread at the routine the C library registered.
// Unless the caller brought its own, the stack is allocated here, with a
// guard page below it and the thread's pthread structure above it.
SysRet Process::sys_bsdthread_create(Thread& t, u32 func, u32 arg, u32 stack, u32 pthread,
                                     u32 flags) {
  if (!thread_start_) return SysRet::err(22);
  std::unique_lock<std::mutex> lk(kmu_);
  u32 top, self, stack_size = stack;
  if (!(flags & 0x01000000)) {  // not PTHREAD_START_CUSTOM
    const u64 total = (u64)stack_size + 4096 + pthread_size_;
    u32 base = mem_.find_free(total, 0xB0000000u, 0xBF000000u);
    if (!base) base = mem_.find_free(total, 0x1000, 0xC0000000u);
    if (!base || !mem_.map(base, total, GuestMemory::kRead | GuestMemory::kWrite)) {
      return SysRet::err(12);
    }
    mem_.protect(base, 4096, 0);
    top = base + 4096 + stack_size;
    self = top;
  } else {
    top = stack;
    self = pthread;
  }
  Thread* nt = new_thread();
  if (arch_ == Arch::PPC) {
    PPCState& s = nt->ppc;
    s.pc = thread_start_;
    s.gpr[1] = top - 64 - 224;  // room for a call frame and the red zone
    s.gpr[3] = self;
    s.sprg3 = self;  // on PowerPC the kernel also installs the thread pointer
    s.gpr[4] = nt->self_port;
    s.gpr[5] = func;
    s.gpr[6] = arg;
    s.gpr[7] = stack_size;
    s.gpr[8] = flags;
  } else {
    X86State& s = nt->x86;
    s.eip = thread_start_;
    s.reg[X86State::EAX] = self;
    s.reg[X86State::EBX] = nt->self_port;
    s.reg[X86State::ECX] = func;
    s.reg[X86State::EDX] = arg;
    s.reg[X86State::EDI] = stack_size;
    s.reg[X86State::ESI] = flags;
    s.reg[X86State::ESP] = top - 16;
  }
  start_thread(nt);
  return SysRet::ok(self);
}
