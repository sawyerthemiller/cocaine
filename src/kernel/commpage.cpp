// The comm page: a few pages the Mac OS X kernel maps at the top of every
// process, holding CPU facts and hand-tuned routines (memcpy, bzero, the
// clock, spin locks) at fixed addresses. The system libraries jump straight
// to those addresses, so the page has to be there.
//
// The layout is the kernel's; the contents are cocaine's own. Most routines
// are a single "host call" followed by a return, which runs the routine as
// native host code: a guest memcpy becomes a real memcpy.

#include <sched.h>
#include <sys/time.h>

#include <ctime>
#include <mutex>

#include "cpu/ppc/ppc_jit.h"
#include "kernel/hooks.h"
#include "kernel/process.h"

namespace {

enum HostCallId : u16 {
  kBzero = 1,
  kBcopy,
  kMemmove,
  kMemsetPattern,
  kGettimeofday,
  kAbsoluteTime,
  kCompareAndSwap32,
  kCompareAndSwap64,
  kAtomicAdd32,
  kAtomicAdd64,
  kEnqueue,
  kDequeue,
  kSpinTry,
  kSpinLock,
  kSpinUnlock,
  kNothing,
  kFlushCode,
};

// Where a thread's per-thread data slots sit inside its pthread structure.
constexpr u32 kTsdOffset = 0x48;

std::mutex g_wide_atomics;

constexpr u32 kPPCBase = 0xFFFF8000u;
constexpr u32 kX86Base = 0xFFFF0000u;

// Capability bits the libraries read to choose code paths.
constexpr u32 kPPCCaps = 0x00000001    // AltiVec
                         | 0x00000004  // 32-byte cache lines
                         | 0x00000100  // data stream instructions exist
                         | 0x00000800  // thread pointer in a user-readable register
                         | (2u << 16)  // two CPUs: guest threads really run in parallel
                         | 0x08000000  // fres, frsqrte, fsel
                         | 0x10000000; // stfiwx
constexpr u32 kX86Caps = 0x00000001    // MMX
                         | 0x00000002  // SSE
                         | 0x00000004  // SSE2
                         | 0x00000008  // SSE3
                         | 0x00000020  // 64-byte cache lines
                         | 0x00000080  // thread pointer in a user-readable register
                         | (2u << 16); // two CPUs: guest threads really run in parallel

}  // namespace

bool Process::setup_commpage(std::string& err) {
  const u8 rwx = GuestMemory::kRead | GuestMemory::kWrite | GuestMemory::kExec;
  if (arch_ == Arch::PPC) {
    const u32 base = kPPCBase;
    if (!mem_.map(base, 0x2000, rwx)) {
      err = "cannot map the comm page";
      return false;
    }
    memcpy(mem_.host(base), "commpage 32-bit", 16);
    mem_.w16(base + 0x01E, 2);  // layout version
    mem_.w32(base + 0x020, kPPCCaps);
    mem_.w8(base + 0x024, 1);     // AltiVec present
    mem_.w8(base + 0x025, 0);     // not a 64-bit CPU
    mem_.w16(base + 0x026, 32);   // cache line size
    mem_.w64(base + 0x040, 0x4330000000000000ull);  // 2^52 as a double
    mem_.w64(base + 0x048, 0x412E848000000000ull);  // 10^6 as a double
    // Constants the string routines use to find a zero byte in a word.
    mem_.w64(base + 0x050, 0xFEFEFEFEFEFEFEFFull);
    mem_.w64(base + 0x058, 0x8080808080808080ull);

    const u32 blr = 0x4E800020u;
    auto routine = [&](u32 offset, u16 id) {
      mem_.w32(base + offset, kPPCHostCall | id);
      mem_.w32(base + offset + 4, blr);
    };
    routine(0x080, kCompareAndSwap32);
    routine(0x0C0, kCompareAndSwap64);
    routine(0x100, kEnqueue);
    routine(0x140, kDequeue);
    routine(0x180, kNothing);  // memory barrier
    routine(0x1A0, kAtomicAdd32);
    routine(0x1C0, kAtomicAdd64);
    routine(0x200, kAbsoluteTime);
    routine(0x220, kSpinTry);
    routine(0x260, kSpinLock);
    routine(0x2A0, kSpinUnlock);
    routine(0x2E0, kGettimeofday);
    routine(0x4E0, kNothing);  // flush data cache
    routine(0x520, kFlushCode);  // flush instruction cache
    routine(0x5C0, kNothing);  // relinquish
    routine(0x600, kBzero);
    routine(0x780, kBcopy);
    routine(0x7A0, kMemmove);
    routine(0xF80, kCompareAndSwap32);
    // Where a guest function cocaine calls returns to (hooks.cpp).
    mem_.w32(kPPCGuestReturnAddr, kPPCHostCall | kGuestReturnCall);
    // The display size, for the window server (see hooks.cpp).
    u32 dw = 0, dh = 0;
    guest_display_size(dw, dh);
    const float size[2] = {(float)dw, (float)dh};
    for (int k = 0; k < 2; k++) {
      u32 bits;
      memcpy(&bits, &size[k], 4);
      mem_.w32(kPPCDisplaySizeAddr + 4 * (u32)k, bits);
    }
    routine(0xFC0, kCompareAndSwap64);
    routine(0x1000, kMemsetPattern);
    routine(0x1140, kNothing);  // bigcopy, only ever reached from the real bcopy

    // pthread_getspecific(key): the thread pointer lives in SPRG3.
    const u32 getspecific[] = {
        0x7CA342A6u,  // mfspr r5, 259
        0x5464103Au,  // slwi  r4, r3, 2
        0x38840000u | kTsdOffset,  // addi r4, r4, kTsdOffset
        0x7C64282Eu,  // lwzx  r3, r4, r5
        blr,
    };
    for (u32 i = 0; i < 5; i++) mem_.w32(base + 0x2C0 + 4 * i, getspecific[i]);
    // pthread_self()
    mem_.w32(base + 0x580, 0x7C6342A6u);  // mfspr r3, 259
    mem_.w32(base + 0x584, blr);
  } else {
    const u32 base = kX86Base;
    if (!mem_.map(base, 0x2000, rwx)) {
      err = "cannot map the comm page";
      return false;
    }
    memcpy(mem_.host(base), "commpage 32-bit", 16);
    mem_.w16(base + 0x01E, 7);  // layout version
    mem_.w32(base + 0x020, kX86Caps);
    mem_.w16(base + 0x026, 64);  // cache line size
    mem_.w64(base + 0x040, 0x4330000000000000ull);  // 2^52 as a double
    mem_.w64(base + 0x048, 0x412E848000000000ull);  // 10^6 as a double

    auto routine = [&](u32 offset, u16 id) {
      const u8 code[] = {0xCD, kX86HostCallVector, (u8)id, (u8)(id >> 8), 0xC3};  // int; id; ret
      memcpy(mem_.host(base + offset), code, sizeof code);
    };
    {  // where a guest function cocaine calls returns to (hooks.cpp)
      const u8 back[4] = {0xCD, kX86HostCallVector, (u8)kGuestReturnCall, (u8)(kGuestReturnCall >> 8)};
      memcpy(mem_.host(kX86GuestReturnAddr), back, 4);
    }
    auto code = [&](u32 offset, std::initializer_list<u8> bytes) {
      u32 p = base + offset;
      for (u8 b : bytes) mem_.w8(p++, b);
    };
    // The atomic primitives are not C functions. The C library loads
    // registers and does `call *ADDRESS`, an indirect call through the first
    // four bytes at ADDRESS, so each slot starts with a pointer to the code
    // that follows it. They are short enough to be real machine code.
    auto atomic = [&](u32 offset, std::initializer_list<u8> body) {
      mem_.w32(base + offset, base + offset + 4);
      code(offset + 4, body);
    };
    // old in eax, new in edx, address in ecx; ZF says whether it swapped
    atomic(0x080, {0xF0, 0x0F, 0xB1, 0x11, 0xC3});  // lock cmpxchg [ecx],edx; ret
    atomic(0xF80, {0xF0, 0x0F, 0xB1, 0x11, 0xC3});
    // old in edx:eax, new in ecx:ebx, address in esi
    atomic(0x0C0, {0xF0, 0x0F, 0xC7, 0x0E, 0xC3});  // lock cmpxchg8b [esi]; ret
    atomic(0xFC0, {0xF0, 0x0F, 0xC7, 0x0E, 0xC3});
    // amount in eax, address in edx; the old value comes back in eax
    atomic(0x1A0, {0xF0, 0x0F, 0xC1, 0x02, 0xC3});  // lock xadd [edx],eax; ret
    // bit number in eax, address in edx; the old bit comes back in CF
    atomic(0x5E0, {0xF0, 0x0F, 0xAB, 0x02, 0xC3});  // lock bts [edx],eax; ret
    atomic(0x5F0, {0xF0, 0x0F, 0xB3, 0x02, 0xC3});  // lock btr [edx],eax; ret
    code(0x180, {0x0F, 0xAE, 0xF0, 0xC3});          // memory barrier: mfence; ret

    routine(0x100, kEnqueue);
    routine(0x140, kDequeue);
    routine(0x200, kAbsoluteTime);
    routine(0x220, kSpinTry);
    routine(0x260, kSpinLock);
    routine(0x2A0, kSpinUnlock);
    routine(0x2E0, kGettimeofday);
    routine(0x4E0, kNothing);  // flush data cache
    routine(0x520, kNothing);  // flush instruction cache
    routine(0x5C0, kNothing);  // relinquish
    routine(0x600, kBzero);
    routine(0x780, kBcopy);
    routine(0x7A0, kMemmove);
    routine(0x1000, kMemsetPattern);
    routine(0x1200, kNothing);  // longcopy, only ever reached from the real bcopy
    routine(0x1700, kAbsoluteTime);  // nanotime

    // pthread_getspecific(key): mov eax,[esp+4]; mov eax,gs:[eax*4+kTsdOffset]; ret
    code(0x2C0, {0x8B, 0x44, 0x24, 0x04, 0x65, 0x8B, 0x04, 0x85, (u8)kTsdOffset, 0, 0, 0, 0xC3});
    // pthread_self(): mov eax,gs:[kTsdOffset]; ret
    code(0x580, {0x65, 0xA1, (u8)kTsdOffset, 0, 0, 0, 0xC3});
  }
  return true;
}

// Every lock a guest thread can hold while running guest code. The forking
// thread holds them all across the fork, so that in the child, where it is the
// only thread, none is left taken by a thread that no longer exists.
void Process::lock_for_fork(bool take) {
  if (take) {
    g_wide_atomics.lock();
    mem_.fork_lock(true);
    x86_bus_lock(true);
    ppc_jit_fork_lock(true);
    x86_jit_fork_lock(true);
    panel_fork_lock(true);
  } else {
    panel_fork_lock(false);
    x86_jit_fork_lock(false);
    ppc_jit_fork_lock(false);
    x86_bus_lock(false);
    mem_.fork_lock(false);
    g_wide_atomics.unlock();
  }
}

// Runs one comm page routine natively. Arguments follow each CPU's normal C
// calling convention unless noted.
void Process::host_call(Thread& t, u32 id) {
  if (id >= kHookCallBase) {  // a library function cocaine supplies itself
    run_hook(t, id - kHookCallBase);
    return;
  }
  if (id == kGuestReturnCall) {
    t.returned = true;
    return;
  }
  SysArgs a;
  u32 esp = 0;
  if (arch_ == Arch::PPC) {
    a.big_endian = true;
    for (int i = 0; i < 8; i++) a.w[i] = t.ppc.gpr[3 + i];
  } else {
    esp = t.x86.reg[X86State::ESP];
    for (int i = 0; i < 8; i++) a.w[i] = mem_.r32(esp + 4 + 4 * (u32)i);
  }
  auto ret32 = [&](u32 v) {
    if (arch_ == Arch::PPC) {
      t.ppc.gpr[3] = v;
    } else {
      t.x86.reg[X86State::EAX] = v;
    }
  };
  auto ret64 = [&](u64 v) {
    if (arch_ == Arch::PPC) {
      t.ppc.gpr[3] = (u32)(v >> 32);
      t.ppc.gpr[4] = (u32)v;
    } else {
      t.x86.reg[X86State::EAX] = (u32)v;
      t.x86.reg[X86State::EDX] = (u32)(v >> 32);
    }
  };

  switch (id) {
    case kBzero: {
      const u32 p = a.u(), n = a.u();
      memset(mem_.host(p), 0, n);
      // Intel's memset(p, 0, n) jumps straight here and hands whatever comes
      // back to its caller, so the pointer has to come back.
      if (arch_ == Arch::I386) ret32(p);
      break;
    }
    case kBcopy: {
      const u32 src = a.u(), dst = a.u(), n = a.u();
      memmove(mem_.host(dst), mem_.host(src), n);
      break;
    }
    case kMemmove: {  // also memcpy; returns the destination
      const u32 dst = a.u(), src = a.u(), n = a.u();
      memmove(mem_.host(dst), mem_.host(src), n);
      ret32(dst);
      break;
    }
    case kMemsetPattern:
      // Not a C function on either CPU: the C library sets up registers and
      // jumps here to fill memory with a repeating 16-byte pattern.
      if (arch_ == Arch::PPC) {
        // r8 = 16-byte-aligned destination, r9 = address of the pattern,
        // r4 = byte count. Stores whole 32-byte chunks and reports what's left.
        PPCState& s = t.ppc;
        const u32 n = s.gpr[4] & ~31u;
        for (u32 i = 0; i < n; i += 16) memcpy(mem_.host(s.gpr[8] + i), mem_.host(s.gpr[9]), 16);
        s.gpr[8] += n;
        s.gpr[4] -= n;
      } else {
        // edi = destination, edx = byte count, xmm0 = the pattern.
        X86State& s = t.x86;
        u8* dst = mem_.host(s.reg[X86State::EDI]);
        const u32 n = s.reg[X86State::EDX];
        memset_pattern16(dst, s.xmm[0].b, n);
      }
      break;
    case kGettimeofday: {  // fills the timeval and returns 0 for "done"
      const u32 tp = a.u();
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      if (tp) {
        mem_.w32(tp, (u32)tv.tv_sec);
        mem_.w32(tp + 4, (u32)tv.tv_usec);
      }
      ret32(0);
      break;
    }
    case kAbsoluteTime:  // one tick per nanosecond, like the CPUs' own clocks here
      ret64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
      break;

    // The atomic operations use host atomics on the guest's memory, so they
    // are atomic against each other and against the CPU cores' own atomic
    // instructions. The 64-bit and queue ones share a lock instead.
    case kCompareAndSwap32: {
      const u32 old_value = a.u(), new_value = a.u(), p = a.u();
      const bool be = mem_.big_endian();
      u32 expect = be ? __builtin_bswap32(old_value) : old_value;
      const u32 desired = be ? __builtin_bswap32(new_value) : new_value;
      ret32(__atomic_compare_exchange_n((u32*)mem_.host(p), &expect, desired, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
      break;
    }
    case kAtomicAdd32: {
      const u32 amount = a.u(), p = a.u();
      const bool be = mem_.big_endian();
      u32* word = (u32*)mem_.host(p);
      u32 seen = __atomic_load_n(word, __ATOMIC_SEQ_CST), sum;
      do {
        sum = (be ? __builtin_bswap32(seen) : seen) + amount;
      } while (!__atomic_compare_exchange_n(word, &seen, be ? __builtin_bswap32(sum) : sum, false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
      ret32(sum);
      break;
    }
    case kCompareAndSwap64: {
      const u64 old_value = a.q(), new_value = a.q();
      const u32 p = a.u();
      std::lock_guard<std::mutex> lk(g_wide_atomics);
      const bool same = mem_.r64(p) == old_value;
      if (same) mem_.w64(p, new_value);
      ret32(same);
      break;
    }
    case kAtomicAdd64: {
      const u64 amount = a.q();
      const u32 p = a.u();
      std::lock_guard<std::mutex> lk(g_wide_atomics);
      const u64 v = mem_.r64(p) + amount;
      mem_.w64(p, v);
      ret64(v);
      break;
    }
    case kEnqueue: {  // push onto a lock-free stack: (list, new element, link offset)
      const u32 list = a.u(), elem = a.u(), offset = a.u();
      std::lock_guard<std::mutex> lk(g_wide_atomics);
      mem_.w32(elem + offset, mem_.r32(list));
      mem_.w32(list, elem);
      break;
    }
    case kDequeue: {  // pop: (list, link offset)
      const u32 list = a.u(), offset = a.u();
      std::lock_guard<std::mutex> lk(g_wide_atomics);
      const u32 head = mem_.r32(list);
      if (head) mem_.w32(list, mem_.r32(head + offset));
      ret32(head);
      break;
    }
    // Spin locks: zero means free. The lock word's "held" value is all ones
    // on Intel and 1 on PowerPC, which reads the same in either byte order
    // only for Intel, so the PowerPC value is stored byte-swapped.
    case kSpinTry:
    case kSpinLock: {
      u32* word = (u32*)mem_.host(a.u());
      const u32 held = arch_ == Arch::PPC ? __builtin_bswap32(1) : 0xFFFFFFFFu;
      for (;;) {
        u32 expect = 0;
        if (__atomic_compare_exchange_n(word, &expect, held, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
          if (id == kSpinTry) ret32(1);
          break;
        }
        if (id == kSpinTry) {
          ret32(0);
          break;
        }
        sched_yield();  // someone else holds it: let them run
      }
      break;
    }
    case kSpinUnlock:
      __atomic_store_n((u32*)mem_.host(a.u()), 0, __ATOMIC_SEQ_CST);
      break;
    case kNothing:
      break;
    case kFlushCode: {  // (address, length): the guest has written code there
      const u32 at = a.u();
      ppc_jit_invalidate(at, a.u());
      break;
    }
    default:
      log_warn("unknown host call %u", id);
      break;
  }
}
