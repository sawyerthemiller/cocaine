// The PowerPC translator: guest code turned into ARM64 code a block at a time.
//
// A block is a straight run of guest instructions, up to the first one that
// always goes somewhere else. Each instruction becomes a few ARM64 ones that
// work on the same PPCState the interpreter uses: guest registers stay in
// that structure, so at any point outside a block the state is complete and
// the interpreter could carry on from it. That is what makes the fallback
// cheap: an instruction with no translation here (anything rare, and AltiVec)
// is done by calling the interpreter for that one instruction.
//
// Blocks are kept for good and found three ways. A branch to a fixed place
// is first a jump back out to ppc_jit_run, which translates the target and
// then rewrites that jump to go straight there, so the next time costs
// nothing. A branch through a register (a return, a call through a pointer)
// looks its target up in a table, in a few instructions, without leaving
// translated code. Anything else goes through ppc_jit_run.
//
// Every block begins by taking its length off a countdown and leaving if
// that has run out, which is how control gets back to the kernel layer to
// deliver signals, and what --max-insns counts.
//
// When guest memory holding translated code is unmapped, mapped over or
// flushed from the cache by the guest (which PowerPC programs must do after
// writing code), the blocks from those pages are forgotten and the jumps
// into them undone. Their ARM64 code is not reused: another thread may still
// be inside it.

#include "cpu/rounding.h"
#include "cpu/ppc/ppc_jit.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include <algorithm>
#include <cfenv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "cpu/ppc/a64.h"

namespace {

using namespace a64;

constexpr u32 kRegionSize = 128u << 20;  // every branch within it reaches
constexpr u32 kTableBits = 16;
constexpr int kMaxBlock = 48;  // guest instructions

// Registers with a fixed job while translated code runs. The rest of the
// callee-saved ones are untouched; w0-w17 are scratch.
constexpr int ST = 19;      // the PPCState
constexpr int MEM = 20;     // host address of guest address 0
constexpr int TBL = 21;     // the table of blocks, for branches through a register
constexpr int BASE = 22;    // where translated code starts
constexpr int BUDGET = 24;  // guest instructions left before going back

// Why translated code came back.
enum Reason : u32 { kLookup, kBudget, kLink, kSyscall, kHostCall, kStop };

#define OFF(field) ((u32)__builtin_offsetof(PPCState, field))
constexpr u32 gpr_at(u32 n) { return OFF(gpr) + 4 * n; }
constexpr u32 fpr_at(u32 n) { return OFF(fpr) + 8 * n; }

constexpr u32 kXerCA = 0x20000000u;

// A jump out of a block to a fixed guest address: the instruction to rewrite
// once the target is translated, and the code it starts out pointing at.
struct Site {
  u32 patch;   // offset of the `b`
  u32 stub;    // offset of the code that asks for the target
  u32 target;  // guest address
};

struct Block {
  u32 pc = 0;
  u32 count = 0;  // guest instructions
  u32 code = 0, code_end = 0;
  bool dead = false;
  std::vector<u32> incoming;  // sites rewritten to jump here
  std::vector<u32> at;        // where each guest instruction's code starts
};

struct Jit {
  std::mutex mu;
  bool tried = false;
  u8* base = nullptr;
  u32 used = 0;
  u64* table = nullptr;  // entries are (guest address << 32) | code offset
  std::deque<Block> blocks;
  std::vector<Site> sites;
  std::unordered_map<u32, u32> by_pc;                 // guest address -> block
  std::map<u32, u32> by_code;                         // code offset -> block
  std::unordered_map<u32, std::vector<u32>> by_page;  // guest page -> blocks
  u32 t_enter = 0, t_exit = 0, t_lookup = 0, t_lookup_kept = 0, t_budget = 0, t_link = 0, t_syscall = 0,
      t_hostcall = 0, t_stop = 0, t_step = 0, t_altivec = 0, t_altivec_mem = 0, t_icbi = 0;
};
Jit g;

// lvsl and lvsr in host order: a vector register's bytes are stored
// reversed (see PPCState::Vec), so guest byte i is host byte 15 - i.
alignas(16) const u8 kDescending15[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
alignas(16) const u8 kDescending31[16] = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};

thread_local PPCStop tl_stop;

// For working on the translator: COCAINE_JIT_STATS=1 counts what leaves
// translated code and which instructions went to the interpreter.
struct Stats {
  bool on = false;
  u64 reasons[6] = {};
  std::mutex mu;
  std::unordered_map<u32, u64> stepped;  // by opcode and extended opcode
};
Stats g_stats;

struct Ret {
  u64 reason;
  s64 left;
};
using Enter = Ret (*)(PPCState*, u8* mem, void* code, s64 budget, u64* table, u8* base);

struct Writing {  // this thread may write translated code, and not run it
  Writing() { pthread_jit_write_protect_np(0); }
  ~Writing() { pthread_jit_write_protect_np(1); }
};

inline u32 guest32(const u8* mem, u32 a) {
  u32 v;
  memcpy(&v, mem + a, 4);
  return __builtin_bswap32(v);
}

// ---- what translated code calls ----------------------------------------------

// One instruction, done by the interpreter. Returns nonzero if it stopped
// for the kernel layer's attention instead (or as well).
u32 helper_step(PPCState* s, u32 pc) {
  s->pc = pc;
  if (__builtin_expect(g_stats.on, 0)) {
    const u32 insn = guest32(s->mem, pc);
    const u32 op = insn >> 26;
    const bool extended = op == 31 || op == 19 || op == 63 || op == 59;
    std::lock_guard<std::mutex> lk(g_stats.mu);
    g_stats.stepped[op << 16 | (extended ? (insn >> 1) & 0x3FF : 0)]++;
  }
  // The instruction is already counted, in the block's length: the
  // interpreter's count of it would be a second one.
  const u64 icount = s->icount;
  t_keep_guest_rounding = true;
  const PPCStop st = ppc_interp(*s, 1);
  t_keep_guest_rounding = false;
  s->icount = icount;
  ppc_apply_rounding(s->fpscr);  // (free: the mode is already the guest's)
  if (st.kind == PPCStopKind::Budget) return 0;
  tl_stop = st;
  return 1;
}

// With COCAINE_JIT_STATS, AltiVec instructions are counted too: opcode 4,
// then the low six bits for the three-operand forms (32 and up) or the low
// eleven otherwise; loads and stores as opcode 31 with their extended opcode.
void count_altivec(u32 insn) {
  const u32 op = insn >> 26;
  const u32 sub = op == 4 ? ((insn & 0x3F) >= 32 ? insn & 0x3F : insn & 0x7FF) : (insn >> 1) & 0x3FF;
  std::lock_guard<std::mutex> lk(g_stats.mu);
  g_stats.stepped[op << 16 | sub]++;
}

u32 helper_altivec(PPCState* s, u32 insn, u32 pc) {
  if (__builtin_expect(g_stats.on, 0)) count_altivec(insn);
  if (altivec_exec(*s, insn)) return 0;
  s->pc = pc;
  tl_stop = {PPCStopKind::Illegal, pc, insn};
  return 1;
}

u32 helper_altivec_mem(PPCState* s, u32 insn, u32 pc) {
  if (__builtin_expect(g_stats.on, 0)) count_altivec(insn);
  if (altivec_memory(*s, insn)) return 0;
  s->pc = pc;
  tl_stop = {PPCStopKind::Illegal, pc, insn};
  return 1;
}

void helper_icbi(PPCState*, u32 address) { ppc_jit_invalidate(address & ~31u, 32); }

// ---- emitting ----------------------------------------------------------------

// Where guest registers live while translated code runs. The busiest are
// kept in host registers the whole time: they are loaded from the PPCState
// on the way in and stored back on every way out (and around every call
// into cocaine), so outside translated code the PPCState is always
// complete. The rest stay in the PPCState and are loaded and stored as
// each instruction needs them.
constexpr int kMap[32] = {
    0,  27, 1,  2,  3,  4,  5,  6,   // r0, r1 (the stack pointer), r2-r7
    7,  8,  9,  10, 11, -1, -1, -1,  // r8-r12
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, 23, 25, 26,  // r29-r31
};
constexpr int CRREG = 28;  // the condition register
// Scratch: values being worked on, and two for the shared sequences below.
constexpr int T0 = 12, T1 = 13, T2 = 14, T3 = 15, S0 = 16, S1 = 17;

struct Emitter {
  u32* p = nullptr;

  void e(u32 word) { *p++ = word; }
  u32 here() const { return (u32)((u8*)p - g.base); }
  void jump(u32 to) { e(b((s64)to - (s64)here())); }
  void call(u32 to) { e(bl((s64)to - (s64)here())); }

  // A constant into a 32-bit register, in one instruction or two.
  void imm(int rd, u32 v) {
    if ((v & 0xFFFF0000u) == 0) {
      e(movzw(rd, v));
    } else if ((v & 0xFFFFu) == 0) {
      e(movzw(rd, v >> 16, 1));
    } else if ((v & 0xFFFF0000u) == 0xFFFF0000u) {
      e(movnw(rd, ~v & 0xFFFF));
    } else if ((v & 0xFFFFu) == 0xFFFFu) {
      e(movnw(rd, (~v >> 16) & 0xFFFF, 1));
    } else {
      e(movzw(rd, v & 0xFFFF));
      e(movkw(rd, v >> 16, 1));
    }
  }
  void address(int rd, const void* where) {
    const u64 a = (u64)where;
    e(movzx(rd, a & 0xFFFF));
    e(movkx(rd, (a >> 16) & 0xFFFF, 1));
    e(movkx(rd, (a >> 32) & 0xFFFF, 2));
    e(movkx(rd, (a >> 48) & 0xFFFF, 3));
  }
  // The registers kept in host registers, to and from the PPCState.
  void spill() {
    for (u32 n = 0; n < 32; n++) {
      if (kMap[n] >= 0) e(strw_at(kMap[n], ST, gpr_at(n)));
    }
    e(strw_at(CRREG, ST, OFF(cr)));
  }
  void fill() {
    for (u32 n = 0; n < 32; n++) {
      if (kMap[n] >= 0) e(ldrw_at(kMap[n], ST, gpr_at(n)));
    }
    e(ldrw_at(CRREG, ST, OFF(cr)));
  }
};

// The code every block shares: getting in, getting out, and the calls.
// Blocks pass these a guest address in w16 and a second word in w17.
void emit_common() {
  Emitter m;
  m.p = (u32*)g.base;
  // Offset 0 is where an empty table entry leads, for a branch to guest
  // address 0: let ppc_run deal with that.
  u32* const zero = m.p;
  m.e(0);

  g.t_exit = m.here();  // w0: the reason
  m.e(movx(1, BUDGET));
  m.e(ldp_at(19, 20, SP, 16));
  m.e(ldp_at(21, 22, SP, 32));
  m.e(ldp_at(23, 24, SP, 48));
  m.e(ldp_at(25, 26, SP, 64));
  m.e(ldp_at(27, 28, SP, 80));
  m.e(ldp_post(29, 30, SP, 96));
  m.e(ret());

  g.t_enter = m.here();  // x0 state, x1 memory, x2 code, x3 budget, x4 table, x5 base
  m.e(stp_pre(29, 30, SP, -96));
  m.e(stp_at(19, 20, SP, 16));
  m.e(stp_at(21, 22, SP, 32));
  m.e(stp_at(23, 24, SP, 48));
  m.e(stp_at(25, 26, SP, 64));
  m.e(stp_at(27, 28, SP, 80));
  m.e(movx(ST, 0));
  m.e(movx(MEM, 1));
  m.e(movx(BUDGET, 3));
  m.e(movx(TBL, 4));
  m.e(movx(BASE, 5));
  m.e(movx(S0, 2));
  m.fill();
  m.e(br(S0));

  // Ways out. w16 is the guest address to go on from, w17 a word to pass along.
  auto leave = [&](bool keep_pc, bool arg, Reason why) {
    const u32 at = m.here();
    m.spill();
    if (!keep_pc) m.e(strw_at(S0, ST, OFF(pc)));
    if (arg) m.e(strw_at(S1, ST, OFF(jit_arg)));
    m.e(movzw(0, why));
    m.jump(g.t_exit);
    return at;
  };
  g.t_lookup = leave(false, false, kLookup);
  g.t_lookup_kept = leave(true, false, kLookup);
  g.t_budget = leave(false, false, kBudget);
  g.t_link = leave(false, true, kLink);
  g.t_syscall = leave(false, true, kSyscall);
  g.t_hostcall = leave(false, true, kHostCall);
  g.t_stop = leave(true, false, kStop);

  // Calls into cocaine, reached with `bl`: routine(state, w17, w16), with
  // the guest's registers in the PPCState for the routine to use and
  // change. What it returns comes back in w16.
  auto calls = [&](const void* routine) {
    const u32 at = m.here();
    m.spill();
    m.e(stp_pre(29, 30, SP, -16));
    m.e(movx(0, ST));
    m.e(movw(1, S1));
    m.e(movw(2, S0));
    m.address(S0, routine);
    m.e(blr(S0));
    m.e(ldp_post(29, 30, SP, 16));
    m.e(movw(S0, 0));
    m.fill();
    m.e(ret());
    return at;
  };
  g.t_step = calls((const void*)&helper_step);
  g.t_altivec = calls((const void*)&helper_altivec);
  g.t_altivec_mem = calls((const void*)&helper_altivec_mem);
  g.t_icbi = calls((const void*)&helper_icbi);

  const u32 for_zero = m.here();
  m.e(movzw(S0, 0));
  m.jump(g.t_lookup);
  *zero = b((s64)for_zero);
  g.used = (m.here() + 63) & ~63u;
}

bool start() {  // with the lock held
  if (g.tried) return g.base != nullptr;
  g.tried = true;
  void* region = mmap(nullptr, kRegionSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (region == MAP_FAILED) return false;
  g.table = (u64*)calloc((size_t)1 << kTableBits, sizeof(u64));
  if (!g.table) return false;
  g.base = (u8*)region;
  Writing w;
  emit_common();
  sys_icache_invalidate(g.base, g.used);
  return true;
}

// ---- translating one block ---------------------------------------------------

class Translator : Emitter {
 public:
  Translator(const u8* mem, u32 pc) : mem_(mem), pc_(pc) { p = (u32*)(g.base + g.used); }

  // Fills in `block`; returns false if the code doesn't fit.
  bool run(Block& block) {
    block.pc = pc_;
    block.code = here();
    // Take the block's length off the countdown; leave if that's all gone.
    u32* const count = p;
    e(0);
    fixes_.push_back({kColdBudget, forward(bcond(MI, 0), 19), pc_, 0, nullptr});

    bool ended = false;
    u32 n = 0;
    while (!ended && n < kMaxBlock) {
      block.at.push_back(here());
      ended = one(guest32(mem_, pc_));
      pc_ += 4;
      n++;
    }
    if (!ended) site(pc_);
    *count = subsx_i(BUDGET, BUDGET, n);
    block.count = n;

    for (const Cold& c : fixes_) {
      bind(c.fix);
      switch (c.kind) {
        case kColdBudget:
          imm(S0, c.a);
          jump(g.t_budget);
          break;
        case kColdSite:
          g.sites[c.b].stub = here();
          imm(S0, c.a);
          imm(S1, c.b);
          jump(g.t_link);
          break;
        case kColdStop:
          jump(g.t_stop);
          break;
        case kColdMiss:
          e(movw(S0, (int)c.a));
          jump(g.t_lookup);
          break;
        case kColdStep:  // this one instruction by the interpreter, then back
          imm(S1, c.a);
          call(g.t_step);
          e(cbzw(S0, (s64)((u8*)c.back - (u8*)p)));
          jump(g.t_stop);
          break;
      }
    }
    block.code_end = here();
    return block.code_end + 4096 < kRegionSize;
  }

 private:
  struct Fix {
    u32* at;
    int bits;  // how wide the instruction's offset field is: 26, 19 or 14
  };
  enum { kColdBudget, kColdSite, kColdStop, kColdMiss, kColdStep };
  struct Cold {
    int kind;
    Fix fix;
    u32 a, b;
    u32* back;
  };

  const u8* mem_;
  u32 pc_;  // of the instruction being translated
  std::vector<Cold> fixes_;

  // A branch whose target comes later.
  Fix forward(u32 word, int bits) {
    Fix f = {p, bits};
    e(word);
    return f;
  }
  void bind(const Fix& f) {
    const u32 words = (u32)(p - f.at);
    if (f.bits == 26) {
      *f.at |= words & 0x03FFFFFF;
    } else if (f.bits == 19) {
      *f.at |= (words & 0x7FFFF) << 5;
    } else {
      *f.at |= (words & 0x3FFF) << 5;
    }
  }

  // ---- guest registers ----
  // R: a host register holding guest register n, to read (never to change):
  // its own if it has one, else `scratch`, loaded. D: where to put a new
  // value for n; W then makes it n's (a store, or nothing at all).
  int R(int n, int scratch) {
    if (kMap[n] >= 0) return kMap[n];
    e(ldrw_at(scratch, ST, gpr_at((u32)n)));
    return scratch;
  }
  int D(int n, int scratch) { return kMap[n] >= 0 ? kMap[n] : scratch; }
  void W(int n, int from) {
    if (kMap[n] < 0) {
      e(strw_at(from, ST, gpr_at((u32)n)));
    } else if (kMap[n] != from) {
      e(movw(kMap[n], from));
    }
  }
  void load_state(int into, u32 offset) { e(ldrw_at(into, ST, offset)); }
  void store_state(int from, u32 offset) { e(strw_at(from, ST, offset)); }

  // ---- arithmetic with constants (S1 is used for ones that don't fit) ----
  void add_imm(int rd, int rn, s32 v) {
    const u32 u = (u32)v, neg = (u32)-v;
    if (v == 0) {
      if (rd != rn) e(movw(rd, rn));
    } else if (u < 4096) {
      e(addw_i(rd, rn, u));
    } else if (neg < 4096) {
      e(subw_i(rd, rn, neg));
    } else if ((u & 0xFFF) == 0 && (u >> 12) < 4096) {
      e(addw_i(rd, rn, u >> 12, true));
    } else if ((neg & 0xFFF) == 0 && (neg >> 12) < 4096) {
      e(subw_i(rd, rn, neg >> 12, true));
    } else {
      imm(S1, u);
      e(addw(rd, rn, S1));
    }
  }
  void and_imm(int rd, int rn, u32 v) {
    if (v == 0) {
      e(movzw(rd, 0));
    } else if (v == 0xFFFFFFFFu) {
      if (rd != rn) e(movw(rd, rn));
    } else if (const Mask m = mask32(v); m.ok) {
      e(andw_i(rd, rn, m));
    } else {
      imm(S1, v);
      e(andw(rd, rn, S1));
    }
  }
  void or_imm(int rd, int rn, u32 v) {
    if (v == 0) {
      if (rd != rn) e(movw(rd, rn));
    } else if (const Mask m = mask32(v); m.ok) {
      e(orrw_i(rd, rn, m));
    } else {
      imm(S1, v);
      e(orrw(rd, rn, S1));
    }
  }
  void xor_imm(int rd, int rn, u32 v) {
    if (v == 0) {
      if (rd != rn) e(movw(rd, rn));
    } else if (const Mask m = mask32(v); m.ok) {
      e(eorw_i(rd, rn, m));
    } else {
      imm(S1, v);
      e(eorw(rd, rn, S1));
    }
  }
  void cmp_imm(int rn, u32 v) {
    if (v < 4096) {
      e(cmpw_i(rn, v));
    } else if ((v & 0xFFF) == 0 && (v >> 12) < 4096) {
      e(subsw_i(ZR, rn, v >> 12, true));
    } else {
      imm(S1, v);
      e(cmpw(rn, S1));
    }
  }

  // ---- the condition register and the carry ----

  // After a host compare: sets one field of CR to less/greater/equal, with
  // the summary-overflow bit copied in from XER.
  void crf(u32 field, bool is_signed) {
    e(movzw(S0, 4));
    e(movzw(S1, 8));
    e(cselw(S0, S1, S0, is_signed ? LT : LO));
    e(movzw(S1, 2));
    e(cselw(S0, S1, S0, EQ));
    load_state(S1, OFF(xer));
    e(orrw_lsr(S0, S0, S1, 31));
    e(bfiw(CRREG, S0, 28 - 4 * field, 4));
  }
  void record(int reg) {  // the "dot" forms: CR0 from the result
    e(cmpw_i(reg, 0));
    crf(0, true);
  }
  void carry_out() {  // XER's carry from the host's carry flag
    e(csetw(S0, HS));
    load_state(S1, OFF(xer));
    e(bfiw(S1, S0, 29, 1));
    store_state(S1, OFF(xer));
  }
  void carry_in() {  // and the other way
    load_state(S0, OFF(xer));
    e(ubfxw(S0, S0, 29, 1));
    e(cmpw_i(S0, 1));
  }

  // ---- guest memory: `addr` holds a guest address ----
  void load32(int rd, int addr) {
    e(ldrw_idx(rd, MEM, addr));
    e(revw(rd, rd));
  }
  void load16(int rd, int addr) {
    e(ldrh_idx(rd, MEM, addr));
    e(rev16w(rd, rd));
  }
  void load8(int rd, int addr) { e(ldrb_idx(rd, MEM, addr)); }
  void store32(int value, int addr) {
    e(revw(S1, value));
    e(strw_idx(S1, MEM, addr));
  }
  void store16(int value, int addr) {
    e(rev16w(S1, value));
    e(strh_idx(S1, MEM, addr));
  }
  void store8(int value, int addr) { e(strb_idx(value, MEM, addr)); }

  // ---- leaving the block ----
  void site(u32 target) {
    const u32 index = (u32)g.sites.size();
    g.sites.push_back({here(), 0, target});
    fixes_.push_back({kColdSite, forward(b(0), 26), target, index, nullptr});
  }
  void leave(u32 next, u32 word, u32 how) {
    imm(S0, next);
    imm(S1, word);
    jump(how);
  }
  // To the guest address in `reg` (a scratch register), by way of the table.
  void indirect(int reg) {
    e(ubfxw(S1, reg, 2, kTableBits));
    e(ldrx_scaled(S1, TBL, S1));
    e(lsrx_i(S0, S1, 32));
    e(cmpw(S0, reg));
    fixes_.push_back({kColdMiss, forward(bcond(NE, 0), 19), (u32)reg, 0, nullptr});
    e(addx_uxtw(S0, BASE, S1));
    e(br(S0));
  }
  // This instruction by the interpreter. `ends`: it may go anywhere after.
  void fallback(bool ends) {
    imm(S1, pc_);
    call(g.t_step);
    fixes_.push_back({kColdStop, forward(cbnzw(S0, 0), 19), 0, 0, nullptr});
    if (ends) jump(g.t_lookup_kept);
  }
  // The same, but from a place only reached in a rare case; `fix` is the
  // branch there. Nothing of the guest's may have been changed yet.
  void rarely(const Fix& fix) { fixes_.push_back({kColdStep, fix, pc_, 0, nullptr}); }
  void rejoin() {  // marks where the rare cases of this instruction come back to
    for (Cold& c : fixes_) {
      if (c.kind == kColdStep && !c.back) c.back = p;
    }
  }
  void vector(u32 insn, u32 how) {
    imm(S1, insn);
    imm(S0, pc_);
    call(how);
    fixes_.push_back({kColdStop, forward(cbnzw(S0, 0), 19), 0, 0, nullptr});
  }

  // ---- AltiVec, done with the host's own vector instructions ----
  // A register is kept byte-reversed in the PPCState, so loaded whole into
  // a NEON register its elements sit in reverse order, each in host byte
  // order: anything done to every element alike is the same instruction,
  // and only what cares where an element sits (permutes, splats, merges,
  // loads and stores) needs its numbering turned round. Registers stay in
  // the PPCState; v0-v4 are scratch. Returns false for what's left to the
  // helper (and so the interpreter), which must then be called.
  static u32 vr_at(u32 n) { return OFF(vr) + 16 * n; }
  void vload(int v, u32 n) { e(ldrq_at(v, ST, vr_at(n))); }
  void vstore(u32 n, int v) { e(strq_at(v, ST, vr_at(n))); }
  // Vector floating point always rounds to nearest, whatever the FPSCR
  // says, and translated code runs in the FPSCR's mode: in the rare case
  // it isn't nearest, the interpreter does the instruction instead. Before
  // anything is changed, so rejoin() must end the instruction.
  void nearest_only() {
    load_state(S0, OFF(fpscr));
    e(andw_i(S0, S0, mask32(3)));
    rarely(forward(cbnzw(S0, 0), 19));
  }
  void splat(u32 size, int v, u32 value) {
    imm(S0, value);
    e(dup_w(size, v, S0));
  }

  bool vector_native(u32 insn) {
    const u32 vd = (insn >> 21) & 31, va = (insn >> 16) & 31, vb = (insn >> 11) & 31, vc = (insn >> 6) & 31;
    if ((insn & 0x3F) >= 32) {  // the three-source forms
      switch (insn & 0x3F) {
        case 42:  // vsel: b where c has a bit, else a
          vload(0, va), vload(1, vb), vload(2, vc);
          e(bslv(2, 1, 0));
          vstore(vd, 2);
          return true;
        case 43:  // vperm. With the table {b, a} in host order, guest byte n of a:b is entry 31 - n.
          vload(2, vb), vload(3, va), vload(1, vc);
          e(mvnv(1, 1));
          splat(0, 4, 31);
          e(andv(1, 1, 4));
          e(tbl2(0, 2, 1));
          vstore(vd, 0);
          return true;
        case 44: {  // vsldoi: a:b shifted left by whole bytes
          const u32 sh = vc & 15;
          vload(0, va);
          if (sh) {
            vload(1, vb);
            e(extv(0, 1, 0, 16 - sh));
          }
          vstore(vd, 0);
          return true;
        }
        case 46:  // vmaddfp: a*c + b, rounded once (NaNs chosen as fmaf(a, c, b) chooses)
          nearest_only();
          vload(0, va), vload(1, vb), vload(2, vc);
          e(fmlav(1, 0, 2));
          vstore(vd, 1);
          rejoin();
          return true;
        case 47:  // vnmsubfp: -(a*c - b), a NaN left as it is (see altivec.cpp)
          nearest_only();
          vload(0, va), vload(1, vb), vload(2, vc);
          e(fnegv(1, 1));
          e(fmlav(1, 0, 2));
          e(fnegv(3, 1));
          e(fcmeqv(false, 4, 1, 1));
          e(bslv(4, 3, 1));
          vstore(vd, 4);
          rejoin();
          return true;
        default:
          return false;
      }
    }
    if ((insn & 0x3F) == 6) {  // compares; with the record bit, CR6 says all (8) or none (2) matched
      u32 word;
      switch (insn & 0x3FF) {
        case 6: word = cmeqv(0, 0, 0, 1); break;          // vcmpequb
        case 70: word = cmeqv(1, 0, 0, 1); break;         // vcmpequh
        case 134: word = cmeqv(2, 0, 0, 1); break;        // vcmpequw
        case 198: word = fcmeqv(false, 0, 0, 1); break;   // vcmpeqfp
        case 454: word = fcmgev(false, 0, 0, 1); break;   // vcmpgefp
        case 710: word = fcmgtv(false, 0, 0, 1); break;   // vcmpgtfp
        case 774: word = cmgtv(0, 0, 0, 1); break;        // vcmpgtsb
        case 838: word = cmgtv(1, 0, 0, 1); break;        // vcmpgtsh
        case 902: word = cmgtv(2, 0, 0, 1); break;        // vcmpgtsw
        default: return false;
      }
      vload(0, va), vload(1, vb);
      e(word);
      vstore(vd, 0);
      if (insn & 0x400) {
        e(fmov_xd(S0, 0));
        e(umov_xd1(S1, 0));
        e(andx(T0, S0, S1));  // all ones: every element matched
        e(orrx(S0, S0, S1));  // zero: none did
        and_imm(CRREG, CRREG, ~0xF0u);
        e(cmnx_i(T0, 1));
        e(csetw(S1, EQ));
        e(orrw(CRREG, CRREG, S1, 7));
        e(cmnx_i(S0, 0));
        e(csetw(S1, EQ));
        e(orrw(CRREG, CRREG, S1, 5));
      }
      return true;
    }
    const u32 xo = insn & 0x7FF;
    auto two = [&](u32 word) {  // vd = a op b, the result built in v0
      vload(0, va), vload(1, vb);
      e(word);
      vstore(vd, 0);
      return true;
    };
    auto one_of_b = [&](u32 word) {  // vd = op b
      vload(1, vb);
      e(word);
      vstore(vd, 0);
      return true;
    };
    switch (xo) {
      case 0: return two(addv(0, 0, 0, 1));        // vaddubm
      case 64: return two(addv(1, 0, 0, 1));       // vadduhm
      case 128: return two(addv(2, 0, 0, 1));      // vadduwm
      case 1024: return two(subv(0, 0, 0, 1));     // vsububm
      case 1088: return two(subv(1, 0, 0, 1));     // vsubuhm
      case 1152: return two(subv(2, 0, 0, 1));     // vsubuwm
      case 1028: return two(andv(0, 0, 1));        // vand
      case 1092: return two(bicv(0, 0, 1));        // vandc
      case 1156: return two(orrv(0, 0, 1));        // vor
      case 1220: return two(eorv(0, 0, 1));        // vxor
      case 1284:                                   // vnor
        vload(0, va), vload(1, vb);
        e(orrv(0, 0, 1));
        e(mvnv(0, 0));
        vstore(vd, 0);
        return true;
      // Merges: the high halves of a and b interleaved, a's first in guest
      // order, are the high halves of b and a interleaved in host order.
      case 12: return two(zip2v(0, 0, 1, 0));      // vmrghb
      case 76: return two(zip2v(1, 0, 1, 0));      // vmrghh
      case 140: return two(zip2v(2, 0, 1, 0));     // vmrghw
      case 268: return two(zip1v(0, 0, 1, 0));     // vmrglb
      case 332: return two(zip1v(1, 0, 1, 0));     // vmrglh
      case 396: return two(zip1v(2, 0, 1, 0));     // vmrglw
      case 524: return one_of_b(dup_elem(0, 0, 1, 15 - (va & 15)));  // vspltb
      case 588: return one_of_b(dup_elem(1, 0, 1, 7 - (va & 7)));    // vsplth
      case 652: return one_of_b(dup_elem(2, 0, 1, 3 - (va & 3)));    // vspltw
      case 780:                                                      // vspltisb
      case 844:                                                      // vspltish
      case 908:                                                      // vspltisw
        splat(xo == 780 ? 0 : xo == 844 ? 1 : 2, 0, (u32)((s32)(va << 27) >> 27));
        vstore(vd, 0);
        return true;
      case 260: case 324: case 388:    // vslb, vslh, vslw: each by its own count, modulo the width
      case 516: case 580: case 644: {  // vsrb, vsrh, vsrw
        const u32 size = (xo >> 6) & 3;
        vload(0, va), vload(1, vb);
        splat(size, 2, (8u << size) - 1);
        e(andv(1, 1, 2));
        if (xo >= 516) e(negv(size, 1, 1));
        e(ushlv(size, 0, 0, 1));
        vstore(vd, 0);
        return true;
      }
      case 8: case 72:       // vmuloub, vmulouh
      case 264: case 328:    // vmulosb, vmulosh
      case 520: case 584:    // vmuleub, vmuleuh
      case 776: case 840: {  // vmulesb, vmulesh
        // Each pair of elements is one element of twice the width here. The
        // odd ones, guest-numbered, are its low half and the even ones its
        // high half: widen the half wanted in place, and the product fits.
        const u32 size = xo & 64 ? 2 : 1, half = 4u << size;
        const bool is_signed = xo & 256, even = xo & 512;
        vload(0, va), vload(1, vb);
        for (int v = 0; v < 2; v++) {
          if (even) {
            e(is_signed ? sshrv(size, v, v, half) : ushrv(size, v, v, half));
          } else if (is_signed) {
            e(shlv(size, v, v, half));
            e(sshrv(size, v, v, half));
          }
        }
        if (!even && !is_signed) {
          splat(size, 2, size == 1 ? 0xFFu : 0xFFFFu);
          e(andv(0, 0, 2));
          e(andv(1, 1, 2));
        }
        e(mulv(size, 0, 0, 1));
        vstore(vd, 0);
        return true;
      }
      case 14:    // vpkuhum
      case 78: {  // vpkuwum: a's elements narrowed then b's, guest order; b's low in host order
        const u32 size = xo == 14 ? 0 : 1;
        vload(0, va), vload(1, vb);
        e(xtnv(size, false, 2, 1));
        e(xtnv(size, true, 2, 0));
        vstore(vd, 2);
        return true;
      }
      case 10:  // vaddfp
      case 74:  // vsubfp
        nearest_only();
        vload(0, va), vload(1, vb);
        e(xo == 10 ? faddv(false, 0, 0, 1) : fsubv(false, 0, 0, 1));
        vstore(vd, 0);
        rejoin();
        return true;
      case 1034:    // vmaxfp
      case 1098: {  // vminfp: a NaN wins as it is, a first; +0 is bigger than -0
        vload(0, va), vload(1, vb);
        e(xo == 1034 ? fmaxv(2, 0, 1) : fminv(2, 0, 1));
        e(fcmeqv(false, 3, 1, 1));
        e(bslv(3, 2, 1));
        e(fcmeqv(false, 4, 0, 0));
        e(bslv(4, 3, 0));
        vstore(vd, 4);
        return true;
      }
      case 266:  // vrefp, and vrsqrtefp: correctly rounded, as the interpreter does them
      case 330:
        nearest_only();
        vload(1, vb);
        if (xo == 330) e(fsqrtv(false, 1, 1));
        e(fmov_one4s(0));
        e(fdivv(false, 0, 0, 1));
        vstore(vd, 0);
        rejoin();
        return true;
      case 522: return one_of_b(frintnv(0, 1));    // vrfin
      case 586: return one_of_b(frintzv(0, 1));    // vrfiz
      case 650: return one_of_b(frintpv(0, 1));    // vrfip
      case 714: return one_of_b(frintmv(0, 1));    // vrfim
      case 778:  // vcfux, vcfsx: words to singles, divided by 2^va
      case 842:
        nearest_only();
        vload(1, vb);
        e(xo == 778 ? ucvtfv_fixed(0, 1, va) : scvtfv_fixed(0, 1, va));
        vstore(vd, 0);
        rejoin();
        return true;
      case 906:     // vctuxs
      case 970: {   // vctsxs: times 2^va, towards zero, saturating, which sets VSCR's SAT
        const bool u = xo == 906;
        vload(1, vb);
        e(u ? fcvtzuv_fixed(0, 1, va) : fcvtzsv_fixed(0, 1, va));
        vstore(vd, 0);
        // Saturated where b*2^va >= 2^32 (2^31), or <= -1 (< -2^31); a NaN never.
        splat(2, 2, ((u ? 159u : 158u) - va) << 23);
        splat(2, 3, 0x80000000u | ((u ? 127u : 158u) - va) << 23);
        e(fcmgev(false, 2, 1, 2));
        e(u ? fcmgev(false, 3, 3, 1) : fcmgtv(false, 3, 3, 1));
        e(orrv(2, 2, 3));
        e(fmov_xd(S0, 2));
        e(umov_xd1(S1, 2));
        e(orrx(S0, S0, S1));
        e(cmnx_i(S0, 0));
        e(csetw(S0, NE));
        load_state(S1, OFF(vscr));
        e(orrw(S1, S1, S0));
        store_state(S1, OFF(vscr));
        return true;
      }
      default:
        return false;
    }
  }

  // lvx, stvx and the permute controls; false for the element loads and
  // stores, left to the helper.
  bool vector_memory_native(u32 insn) {
    const u32 vd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
    switch ((insn >> 1) & 0x3FF) {
      case 103:
      case 359: {  // lvx, lvxl: sixteen bytes from the aligned address, reversed
        and_imm(T0, ea_x(ra, rb), ~15u);
        e(ldrq_idx(0, MEM, T0));
        e(rev64v(0, 0));
        e(extv(0, 0, 0, 8));
        vstore(vd, 0);
        return true;
      }
      case 231:
      case 487: {  // stvx, stvxl
        and_imm(T0, ea_x(ra, rb), ~15u);
        vload(0, vd);
        e(rev64v(0, 0));
        e(extv(0, 0, 0, 8));
        e(strq_idx(0, MEM, T0));
        return true;
      }
      case 6:     // lvsl: guest byte i is sh + i
      case 38: {  // lvsr: 16 - sh + i
        and_imm(T0, ea_x(ra, rb), 15);
        e(dup_w(0, 1, T0));
        address(S1, ((insn >> 1) & 0x3FF) == 6 ? kDescending15 : kDescending31);
        e(ldrq_at(0, S1, 0));
        e(((insn >> 1) & 0x3FF) == 6 ? addv(0, 0, 0, 1) : subv(0, 0, 0, 1));
        vstore(vd, 0);
        return true;
      }
      default:
        return false;
    }
  }

  // The branch conditions shared by bc, bclr and bcctr. Returns the branches
  // taken when the guest's branch is not, to be bound after it.
  std::vector<Fix> conditions(u32 bo, u32 bi, bool counts) {
    std::vector<Fix> skips;
    if (counts && !(bo & 4)) {
      load_state(S0, OFF(ctr));
      e(subw_i(S0, S0, 1));
      store_state(S0, OFF(ctr));
      skips.push_back(forward((bo & 2) ? cbnzw(S0, 0) : cbzw(S0, 0), 19));
    }
    if (!(bo & 16)) {
      const u32 bit = 31 - bi;
      skips.push_back(forward((bo & 8) ? tbzw(CRREG, bit, 0) : tbnzw(CRREG, bit, 0), 14));
    }
    return skips;
  }

  static u32 ppc_mask(u32 mb, u32 me) {
    const u32 a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
  }

  // Effective addresses. The register returned may be a guest's own (when
  // the address is just that register), so it is only to be read; `own`
  // asks for it in T0 regardless, for the forms that go on to change it.
  int ea_d(int ra, s32 d, bool own = false) {
    if (!ra) {
      imm(T0, (u32)d);
      return T0;
    }
    const int a = R(ra, T0);
    if (d == 0 && !own) return a;
    add_imm(T0, a, d);
    return T0;
  }
  int ea_x(int ra, int rb, bool own = false) {
    if (!ra) {
      const int b = R(rb, T0);
      if (own && b != T0) e(movw(T0, b));
      return own ? T0 : b;
    }
    const int a = R(ra, T0), b = R(rb, T1);
    e(addw(T0, a, b));
    return T0;
  }

  // ---- floating point: the registers stay in the PPCState ----
  void FR(int n, int into) { e(ldrd_at(into, ST, fpr_at((u32)n))); }
  void FW(int n, int from) { e(strd_at(from, ST, fpr_at((u32)n))); }
  void round_single(int d) {
    e(fcvt_sd(d, d));
    e(fcvt_ds(d, d));
  }
  // lfs: the single at guest address `ea` into FPR `rd`, widened. Infinities
  // and NaNs are moved bit by bit, which a host conversion wouldn't do.
  void load_single(int rd, int ea) {
    load32(T1, ea);
    e(andw_i(T2, T1, mask32(0x7F800000u)));
    imm(T3, 0x7F800000u);
    e(cmpw(T2, T3));
    rarely(forward(bcond(EQ, 0), 19));
    e(fmov_sw(0, T1));
    e(fcvt_ds(0, 0));
    FW(rd, 0);
  }
  // stfs: FPR `rs` to the guest address `ea` as a single, by moving bits
  // (not rounding): exact whenever the value is a single already.
  void store_single(int rs, int ea) {
    e(ldrx_at(S0, ST, fpr_at((u32)rs)));
    e(ubfxx(S1, S0, 52, 11));
    e(cmpw_i(S1, 896));
    const Fix normal = forward(bcond(HI, 0), 19);
    e(lslx_i(S1, S0, 1));
    rarely(forward(cbnzx(S1, 0), 19));  // tiny but not zero: the slow way
    bind(normal);
    e(lsrx_i(S1, S0, 32));
    e(andw_i(S1, S1, mask32(0xC0000000u)));
    e(ubfxx(S0, S0, 29, 30));
    e(orrw(S0, S0, S1));
    e(revw(S0, S0));
    e(strw_idx(S0, MEM, ea));
  }
  void load_double(int rd, int ea) {
    e(ldrx_idx(S0, MEM, ea));
    e(revx(S0, S0));
    e(strx_at(S0, ST, fpr_at((u32)rd)));
  }
  void store_double(int rs, int ea) {
    e(ldrx_at(S0, ST, fpr_at((u32)rs)));
    e(revx(S0, S0));
    e(strx_idx(S0, MEM, ea));
  }

  // ---- one guest instruction. Returns true if the block ends with it. ----
  bool one(u32 insn);
  bool op19(u32 insn);
  bool op31(u32 insn);
  bool op59(u32 insn);
  bool op63(u32 insn);
};

bool Translator::one(u32 insn) {
  const u32 op = insn >> 26;
  const int rd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
  const int rs = rd;
  const s32 simm = (s16)insn;
  const u32 uimm = insn & 0xFFFF;
  const u32 crfd = (insn >> 23) & 7;

  // The loads and stores that take a register and a constant. `update`
  // forms leave the address in rA afterwards.
  auto load = [&](int size, bool update, bool sign) {
    const int ea = update ? ea_d(ra, simm, true) : ea_d(ra, simm);
    const int d = D(rd, T1);
    if (size == 4) {
      load32(d, ea);
    } else if (size == 2) {
      load16(d, ea);
      if (sign) e(sxthw(d, d));
    } else {
      load8(d, ea);
    }
    W(rd, d);
    if (update) W(ra, T0);
    return false;
  };
  auto store = [&](int size, bool update) {
    const int ea = update ? ea_d(ra, simm, true) : ea_d(ra, simm);
    const int v = R(rs, T1);
    if (size == 4) {
      store32(v, ea);
    } else if (size == 2) {
      store16(v, ea);
    } else {
      store8(v, ea);
    }
    if (update) W(ra, T0);
    return false;
  };
  // rA = rS op constant
  auto logic = [&](void (Translator::*apply)(int, int, u32), u32 v, bool dot) {
    const int s = R(rs, T0), d = D(ra, T0);
    (this->*apply)(d, s, v);
    W(ra, d);
    if (dot) record(d);
    return false;
  };

  switch (op) {
    case 1:  // a host call
      leave(pc_ + 4, insn, g.t_hostcall);
      return true;
    case 7: {  // mulli
      const int a = R(ra, T0), d = D(rd, T0);
      imm(T1, (u32)simm);
      e(mulw(d, a, T1));
      W(rd, d);
      return false;
    }
    case 8: {  // subfic
      const int a = R(ra, T0), d = D(rd, T0);
      imm(T1, (u32)simm);
      e(subsw(d, T1, a));
      W(rd, d);
      carry_out();
      return false;
    }
    case 10:  // cmpli
      cmp_imm(R(ra, T0), uimm);
      crf(crfd, false);
      return false;
    case 11:  // cmpi
      cmp_imm(R(ra, T0), (u32)simm);
      crf(crfd, true);
      return false;
    case 12:
    case 13: {  // addic, addic.
      const int a = R(ra, T0), d = D(rd, T0);
      const u32 value = (u32)simm;
      if (value < 4096) {
        e(addsw_i(d, a, value));
      } else if ((value & 0xFFF) == 0 && (value >> 12) < 4096) {
        e(addsw_i(d, a, value >> 12, true));
      } else {
        imm(T1, value);
        e(addsw(d, a, T1));
      }
      W(rd, d);
      carry_out();
      if (op == 13) record(d);
      return false;
    }
    case 14:    // addi
    case 15: {  // addis
      const u32 v = op == 14 ? (u32)simm : uimm << 16;
      const int d = D(rd, T0);
      if (ra) {
        add_imm(d, R(ra, T0), (s32)v);
      } else {
        imm(d, v);
      }
      W(rd, d);
      return false;
    }

    case 16: {  // bc
      const u32 bo = (u32)rd, bi = (u32)ra;
      const u32 bd = (u32)(s32)(s16)(insn & 0xFFFC);
      const u32 target = (insn & 2) ? bd : pc_ + bd;
      if (insn & 1) {
        imm(T0, pc_ + 4);
        store_state(T0, OFF(lr));
      }
      const std::vector<Fix> skips = conditions(bo, bi, true);
      site(target);
      for (const Fix& f : skips) bind(f);
      return skips.empty();
    }
    case 17:  // sc
      leave(pc_ + 4, insn, g.t_syscall);
      return true;
    case 18: {  // b
      const u32 li = (u32)(((s32)(insn << 6)) >> 6) & ~3u;
      if (insn & 1) {
        imm(T0, pc_ + 4);
        store_state(T0, OFF(lr));
      }
      site((insn & 2) ? li : pc_ + li);
      return true;
    }
    case 19:
      return op19(insn);

    case 20: {  // rlwimi
      const u32 sh = (u32)rb, mask = ppc_mask((insn >> 6) & 31, (insn >> 1) & 31);
      int s = R(rs, T0);
      const int a = R(ra, T1), d = D(ra, T0);
      if (sh) {
        e(rorw_i(T2, s, 32 - sh));
        s = T2;
      }
      and_imm(T2, s, mask);
      and_imm(T3, a, ~mask);
      e(orrw(d, T2, T3));
      W(ra, d);
      if (insn & 1) record(d);
      return false;
    }
    case 21: {  // rlwinm
      const u32 sh = (u32)rb, mb = (insn >> 6) & 31, me = (insn >> 1) & 31;
      int s = R(rs, T0);
      const int d = D(ra, T0);
      if (sh && mb == 0 && me == 31 - sh) {
        e(lslw_i(d, s, sh));  // a plain shift left
      } else if (sh && me == 31 && mb == 32 - sh) {
        e(lsrw_i(d, s, 32 - sh));  // and right
      } else {
        if (sh) {
          e(rorw_i(T1, s, 32 - sh));
          s = T1;
        }
        and_imm(d, s, ppc_mask(mb, me));
      }
      W(ra, d);
      if (insn & 1) record(d);
      return false;
    }
    case 23: {  // rlwnm
      const u32 mask = ppc_mask((insn >> 6) & 31, (insn >> 1) & 31);
      const int s = R(rs, T0), n = R(rb, T1), d = D(ra, T0);
      e(negw(T2, n));  // left by n is right by 32 - n; only five bits count
      e(rorvw(T2, s, T2));
      and_imm(d, T2, mask);
      W(ra, d);
      if (insn & 1) record(d);
      return false;
    }
    case 24:  // ori
      if (uimm == 0 && rs == ra) return false;  // the usual no-op
      return logic(&Translator::or_imm, uimm, false);
    case 25: return logic(&Translator::or_imm, uimm << 16, false);   // oris
    case 26: return logic(&Translator::xor_imm, uimm, false);        // xori
    case 27: return logic(&Translator::xor_imm, uimm << 16, false);  // xoris
    case 28: return logic(&Translator::and_imm, uimm, true);         // andi.
    case 29: return logic(&Translator::and_imm, uimm << 16, true);   // andis.
    case 31:
      return op31(insn);

    case 32: return load(4, false, false);  // lwz
    case 33: return load(4, true, false);   // lwzu
    case 34: return load(1, false, false);  // lbz
    case 35: return load(1, true, false);   // lbzu
    case 36: return store(4, false);        // stw
    case 37: return store(4, true);         // stwu
    case 38: return store(1, false);        // stb
    case 39: return store(1, true);         // stbu
    case 40: return load(2, false, false);  // lhz
    case 41: return load(2, true, false);   // lhzu
    case 42: return load(2, false, true);   // lha
    case 43: return load(2, true, true);    // lhau
    case 44: return store(2, false);        // sth
    case 45: return store(2, true);         // sthu
    case 46: {  // lmw
      ea_d(ra, simm, true);
      for (int r = rd; r < 32; r++) {
        const int d = D(r, T1);
        load32(d, T0);
        W(r, d);
        if (r < 31) e(addw_i(T0, T0, 4));
      }
      return false;
    }
    case 47: {  // stmw
      ea_d(ra, simm, true);
      for (int r = rs; r < 32; r++) {
        store32(R(r, T1), T0);
        if (r < 31) e(addw_i(T0, T0, 4));
      }
      return false;
    }

    // -- floating point loads and stores --
    case 48:    // lfs
    case 49: {  // lfsu
      const bool update = op == 49;
      load_single(rd, ea_d(ra, simm, update));
      if (update) W(ra, T0);
      rejoin();
      return false;
    }
    case 50:    // lfd
    case 51: {  // lfdu
      const bool update = op == 51;
      load_double(rd, ea_d(ra, simm, update));
      if (update) W(ra, T0);
      return false;
    }
    case 52:    // stfs
    case 53: {  // stfsu
      const bool update = op == 53;
      store_single(rs, ea_d(ra, simm, update));
      if (update) W(ra, T0);
      rejoin();
      return false;
    }
    case 54:    // stfd
    case 55: {  // stfdu
      const bool update = op == 55;
      store_double(rs, ea_d(ra, simm, update));
      if (update) W(ra, T0);
      return false;
    }

    case 59:
      return op59(insn);
    case 63:
      return op63(insn);
    case 4:  // AltiVec
      if (!vector_native(insn)) vector(insn, g.t_altivec);
      return false;
    default:
      fallback(true);
      return true;
  }
}

bool Translator::op19(u32 insn) {
  const u32 xo = (insn >> 1) & 0x3FF;
  const u32 bt = (insn >> 21) & 31, ba = (insn >> 16) & 31, bb = (insn >> 11) & 31;
  switch (xo) {
    case 0: {  // mcrf
      const u32 crfd = (insn >> 23) & 7, crfs = (insn >> 18) & 7;
      e(ubfxw(S0, CRREG, 28 - 4 * crfs, 4));
      e(bfiw(CRREG, S0, 28 - 4 * crfd, 4));
      return false;
    }
    case 16:     // bclr
    case 528: {  // bcctr
      load_state(T0, xo == 16 ? OFF(lr) : OFF(ctr));
      if (insn & 1) {
        imm(T1, pc_ + 4);
        store_state(T1, OFF(lr));
      }
      const std::vector<Fix> skips = conditions(bt, ba, xo == 16);
      e(andw_i(T0, T0, mask32(0xFFFFFFFCu)));
      indirect(T0);
      for (const Fix& f : skips) bind(f);
      return skips.empty();
    }
    case 33:
    case 129:
    case 193:
    case 225:
    case 257:
    case 289:
    case 417:
    case 449: {  // the logic on single CR bits
      if (bt == ba && ba == bb && (xo == 193 || xo == 289)) {
        // One bit cleared or set: how the compilers flag whether a call
        // with a variable argument list passes any floating point.
        if (xo == 193) {
          and_imm(CRREG, CRREG, ~(1u << (31 - bt)));
        } else {
          or_imm(CRREG, CRREG, 1u << (31 - bt));
        }
        return false;
      }
      e(ubfxw(T0, CRREG, 31 - ba, 1));
      e(ubfxw(T1, CRREG, 31 - bb, 1));
      switch (xo) {
        case 33: e(orrw(T0, T0, T1)); e(mvnw(T0, T0)); break;   // crnor
        case 129: e(bicw(T0, T0, T1)); break;                   // crandc
        case 193: e(eorw(T0, T0, T1)); break;                   // crxor
        case 225: e(andw(T0, T0, T1)); e(mvnw(T0, T0)); break;  // crnand
        case 257: e(andw(T0, T0, T1)); break;                   // crand
        case 289: e(eonw(T0, T0, T1)); break;                   // creqv
        case 417: e(ornw(T0, T0, T1)); break;                   // crorc
        default: e(orrw(T0, T0, T1)); break;                    // cror
      }
      e(bfiw(CRREG, T0, 31 - bt, 1));
      return false;
    }
    case 150:  // isync
      return false;
    default:
      fallback(true);
      return true;
  }
}

bool Translator::op31(u32 insn) {
  const u32 xo = (insn >> 1) & 0x3FF;
  const int rd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
  const int rs = rd;
  const bool rc = insn & 1;
  const u32 crfd = (insn >> 23) & 7;

  // rA = rS op rB; `word` is given the three host registers
  auto logic = [&](u32 (*word)(int, int, int), bool invert) {
    const int s = R(rs, T0), b = R(rb, T1), d = D(ra, T0);
    e(word(d, s, b));
    if (invert) e(mvnw(d, d));
    W(ra, d);
    if (rc) record(d);
    return false;
  };
  // rA = op rS
  auto unary = [&](u32 (*word)(int, int)) {
    const int s = R(rs, T0), d = D(ra, T0);
    e(word(d, s));
    W(ra, d);
    if (rc) record(d);
    return false;
  };
  // rD = rA op rB, the add and subtract family, with the carry in or out
  auto sum = [&](u32 (*word)(int, int, int), bool takes_carry, bool gives_carry) {
    if (takes_carry) carry_in();  // first: it disturbs nothing the operands need
    const int a = R(ra, T0), b = R(rb, T1), d = D(rd, T0);
    e(word(d, a, b));
    W(rd, d);
    if (gives_carry) carry_out();
    if (rc) record(d);
    return false;
  };
  auto sum1 = [&](u32 (*word)(int, int), bool takes_carry, bool gives_carry) {
    if (takes_carry) carry_in();
    const int a = R(ra, T0), d = D(rd, T0);
    e(word(d, a));
    W(rd, d);
    if (gives_carry) carry_out();
    if (rc) record(d);
    return false;
  };
  // loads and stores with the address from two registers
  auto load = [&](int size, bool update, bool sign, bool reversed) {
    const int ea = ea_x(ra, rb, update);
    const int d = D(rd, T1);
    if (size == 4) {
      e(ldrw_idx(d, MEM, ea));
      if (!reversed) e(revw(d, d));
    } else if (size == 2) {
      e(ldrh_idx(d, MEM, ea));
      if (!reversed) e(rev16w(d, d));
      if (sign) e(sxthw(d, d));
    } else {
      load8(d, ea);
    }
    W(rd, d);
    if (update) W(ra, T0);
    return false;
  };
  auto store = [&](int size, bool update, bool reversed) {
    const int ea = ea_x(ra, rb, update);
    const int v = R(rs, T2);
    if (size == 4) {
      if (reversed) {
        e(strw_idx(v, MEM, ea));
      } else {
        store32(v, ea);
      }
    } else if (size == 2) {
      if (reversed) {
        e(strh_idx(v, MEM, ea));
      } else {
        store16(v, ea);
      }
    } else {
      store8(v, ea);
    }
    if (update) W(ra, T0);
    return false;
  };

  switch (xo) {
    case 0:     // cmp
    case 32: {  // cmpl
      const int a = R(ra, T0), b = R(rb, T1);
      e(cmpw(a, b));
      crf(crfd, xo == 0);
      return false;
    }

    case 266: return sum([](int d, int a, int b) { return addw(d, a, b); }, false, false);   // add
    case 40: return sum([](int d, int a, int b) { return subw(d, b, a); }, false, false);    // subf
    case 104: return sum1([](int d, int a) { return negw(d, a); }, false, false);            // neg
    case 10: return sum([](int d, int a, int b) { return addsw(d, a, b); }, false, true);    // addc
    case 8: return sum([](int d, int a, int b) { return subsw(d, b, a); }, false, true);     // subfc
    case 138: return sum([](int d, int a, int b) { return adcsw(d, a, b); }, true, true);    // adde
    case 136: return sum([](int d, int a, int b) { return sbcsw(d, b, a); }, true, true);    // subfe
    case 202: return sum1([](int d, int a) { return adcsw(d, a, ZR); }, true, true);         // addze
    case 200: return sum1([](int d, int a) { return sbcsw(d, ZR, a); }, true, true);         // subfze
    case 235: return sum([](int d, int a, int b) { return mulw(d, a, b); }, false, false);   // mullw
    case 459: return sum([](int d, int a, int b) { return udivw(d, a, b); }, false, false);  // divwu: 0 for a zero divisor on both
    case 491: {  // divw. The host's answer to a bad divide differs: a zero divisor, or -1
                 // (whose one overflowing case is rare to test for), go to the interpreter.
      const int a = R(ra, T0), b = R(rb, T1), d = D(rd, T0);
      rarely(forward(cbzw(b, 0), 19));
      e(addsw_i(ZR, b, 1));
      rarely(forward(bcond(EQ, 0), 19));
      e(sdivw(d, a, b));
      W(rd, d);
      if (rc) record(d);
      rejoin();
      return false;
    }
    case 792: {  // sraw: done in 64 bits, where a count of 32 to 63 still shifts
      const int s = R(rs, T0), n = R(rb, T1), d = D(ra, T0);
      e(sxtw(T3, s));
      e(andw_i(T2, n, mask32(63)));
      e(asrvx(S0, T3, T2));
      // The carry: a negative value that loses one bits.
      e(lslvx(S1, S0, T2));
      e(cmpx(S1, T3));
      e(csetw(S1, NE));
      e(andw_lsr(S1, S1, T3, 31));
      load_state(T2, OFF(xer));
      e(bfiw(T2, S1, 29, 1));
      store_state(T2, OFF(xer));
      e(movw(d, S0));
      W(ra, d);
      if (rc) record(d);
      return false;
    }
    case 75:     // mulhw
    case 11: {   // mulhwu
      const int a = R(ra, T0), b = R(rb, T1);
      e(xo == 75 ? smull(T0, a, b) : umull(T0, a, b));
      e(lsrx_i(T0, T0, 32));
      W(rd, T0);
      if (rc) record(T0);
      return false;
    }

    case 28: return logic([](int d, int s, int b) { return andw(d, s, b); }, false);   // and
    case 60: return logic([](int d, int s, int b) { return bicw(d, s, b); }, false);   // andc
    case 444:  // or, which is also how a register is copied
      if (rs == rb) {
        const int s = R(rs, T0);
        W(ra, s);
        if (rc) record(s);
        return false;
      }
      return logic([](int d, int s, int b) { return orrw(d, s, b); }, false);
    case 412: return logic([](int d, int s, int b) { return ornw(d, s, b); }, false);  // orc
    case 316: return logic([](int d, int s, int b) { return eorw(d, s, b); }, false);  // xor
    case 476: return logic([](int d, int s, int b) { return andw(d, s, b); }, true);   // nand
    case 124: return logic([](int d, int s, int b) { return orrw(d, s, b); }, true);   // nor
    case 284: return logic([](int d, int s, int b) { return eonw(d, s, b); }, false);  // eqv
    case 954: return unary([](int d, int s) { return sxtbw(d, s); });                  // extsb
    case 922: return unary([](int d, int s) { return sxthw(d, s); });                  // extsh
    case 26: return unary([](int d, int s) { return clzw(d, s); });                    // cntlzw

    case 24:     // slw
    case 536: {  // srw: a count of 32 or more gives 0, which the host's doesn't
      const int s = R(rs, T0), n = R(rb, T1), d = D(ra, T0);
      e(xo == 24 ? lslvw(T2, s, n) : lsrvw(T2, s, n));
      e(tstw_i(n, mask32(32)));
      e(cselw(d, ZR, T2, NE));
      W(ra, d);
      if (rc) record(d);
      return false;
    }
    case 824: {  // srawi
      const u32 n = (u32)rb;
      const int s = R(rs, T0), d = D(ra, T0);
      if (n == 0) {
        load_state(S1, OFF(xer));
        e(andw_i(S1, S1, mask32(~kXerCA)));
        store_state(S1, OFF(xer));
        W(ra, s);
        if (rc) record(s);
        return false;
      }
      // The carry: a negative value that loses one bits.
      e(tstw_i(s, mask32((1u << n) - 1)));
      e(csetw(S0, NE));
      e(andw_lsr(S0, S0, s, 31));
      load_state(S1, OFF(xer));
      e(bfiw(S1, S0, 29, 1));
      store_state(S1, OFF(xer));
      e(asrw_i(d, s, n));
      W(ra, d);
      if (rc) record(d);
      return false;
    }

    case 19:  // mfcr
      W(rd, CRREG);
      return false;
    case 144: {  // mtcrf
      const u32 crm = (insn >> 12) & 0xFF;
      u32 mask = 0;
      for (int i = 0; i < 8; i++) {
        if (crm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
      }
      const int s = R(rs, T0);
      if (mask == 0xFFFFFFFFu) {
        e(movw(CRREG, s));
      } else {
        and_imm(T1, s, mask);
        and_imm(CRREG, CRREG, ~mask);
        e(orrw(CRREG, CRREG, T1));
      }
      return false;
    }
    case 339: {  // mfspr
      const u32 spr = ((insn >> 16) & 0x1F) | ((insn >> 6) & 0x3E0);
      u32 from = 0;
      switch (spr) {
        case 1: from = OFF(xer); break;
        case 8: from = OFF(lr); break;
        case 9: from = OFF(ctr); break;
        case 256: from = OFF(vrsave); break;
        case 259: from = OFF(sprg3); break;
        default:  // the clock, or one that isn't there
          fallback(false);
          return false;
      }
      const int d = D(rd, T0);
      load_state(d, from);
      W(rd, d);
      return false;
    }
    case 467: {  // mtspr
      const u32 spr = ((insn >> 16) & 0x1F) | ((insn >> 6) & 0x3E0);
      if (spr != 1 && spr != 8 && spr != 9 && spr != 256) {
        fallback(false);
        return false;
      }
      const int s = R(rs, T0);
      if (spr == 1) {
        and_imm(T1, s, 0xE000007Fu);
        store_state(T1, OFF(xer));
      } else {
        store_state(s, spr == 8 ? OFF(lr) : spr == 9 ? OFF(ctr) : OFF(vrsave));
      }
      return false;
    }

    case 23: return load(4, false, false, false);   // lwzx
    case 55: return load(4, true, false, false);    // lwzux
    case 87: return load(1, false, false, false);   // lbzx
    case 119: return load(1, true, false, false);   // lbzux
    case 279: return load(2, false, false, false);  // lhzx
    case 311: return load(2, true, false, false);   // lhzux
    case 343: return load(2, false, true, false);   // lhax
    case 375: return load(2, true, true, false);    // lhaux
    case 534: return load(4, false, false, true);   // lwbrx
    case 790: return load(2, false, false, true);   // lhbrx
    case 151: return store(4, false, false);        // stwx
    case 183: return store(4, true, false);         // stwux
    case 215: return store(1, false, false);        // stbx
    case 247: return store(1, true, false);         // stbux
    case 407: return store(2, false, false);        // sthx
    case 439: return store(2, true, false);         // sthux
    case 662: return store(4, false, true);         // stwbrx
    case 918: return store(2, false, true);         // sthbrx

    // -- hints about caches and ordering: nothing to do --
    case 54:
    case 86:
    case 246:
    case 278:
    case 598:
    case 854:
    case 342:
    case 374:
    case 822:
      return false;
    case 982: {  // icbi: the guest has changed code at this address
      e(movw(S1, ea_x(ra, rb)));
      call(g.t_icbi);
      imm(S0, pc_ + 4);
      jump(g.t_lookup);
      return true;
    }
    case 1014: {  // dcbz: 32 bytes of zeroes
      const int ea = ea_x(ra, rb);
      e(andw_i(T0, ea, mask32(0xFFFFFFE0u)));
      for (int i = 0; i < 4; i++) {
        e(strx_idx(ZR, MEM, T0));
        if (i < 3) e(addw_i(T0, T0, 8));
      }
      return false;
    }

    // -- floating point loads and stores, indexed --
    case 535:  // lfsx
      load_single(rd, ea_x(ra, rb));
      rejoin();
      return false;
    case 599:  // lfdx
      load_double(rd, ea_x(ra, rb));
      return false;
    case 663:  // stfsx
      store_single(rs, ea_x(ra, rb));
      rejoin();
      return false;
    case 727:  // stfdx
      store_double(rs, ea_x(ra, rb));
      return false;
    case 983: {  // stfiwx
      const int ea = ea_x(ra, rb);
      e(ldrx_at(S0, ST, fpr_at((u32)rs)));
      store32(S0, ea);
      return false;
    }

    // -- AltiVec loads, stores and shift patterns --
    case 6:
    case 38:
    case 7:
    case 39:
    case 71:
    case 103:
    case 359:
    case 135:
    case 167:
    case 199:
    case 231:
    case 487:
      if (!vector_memory_native(insn)) vector(insn, g.t_altivec_mem);
      return false;

    // Everything else this group has: the forms that record overflow, the
    // atomic pair, string moves, the clock. None goes anywhere but the next
    // instruction, unless it traps, which the interpreter then reports.
    default:
      fallback(false);
      return false;
  }
}

bool Translator::op59(u32 insn) {  // single precision arithmetic
  const int fd = (insn >> 21) & 31, fa = (insn >> 16) & 31, fb = (insn >> 11) & 31, fc = (insn >> 6) & 31;
  if (insn & 1) {
    fallback(false);
    return false;
  }
  switch ((insn >> 1) & 31) {
    case 18: FR(fa, 0); FR(fb, 1); e(fdivd(0, 0, 1)); break;  // fdivs
    case 20: FR(fa, 0); FR(fb, 1); e(fsubd(0, 0, 1)); break;  // fsubs
    case 21: FR(fa, 0); FR(fb, 1); e(faddd(0, 0, 1)); break;  // fadds
    case 22: FR(fb, 0); e(fsqrtd(0, 0)); break;               // fsqrts
    case 25: FR(fa, 0); FR(fc, 1); e(fmuld(0, 0, 1)); break;  // fmuls
    case 28:    // fmsubs
    case 29:    // fmadds
    case 30:    // fnmsubs
    case 31: {  // fnmadds
      // Rounded once, as singles, when all three inputs are singles: the
      // normal case. Anything else (a NaN among them, or a double) is left
      // to the interpreter, which does it the slow way.
      const u32 xo = (insn >> 1) & 31;
      FR(fa, 0);
      FR(fc, 1);
      FR(fb, 2);
      for (int r = 0; r < 3; r++) {
        e(fcvt_sd(3, r));
        e(fcvt_ds(3, 3));
        e(fcmpd(3, r));
        rarely(forward(bcond(NE, 0), 19));
      }
      for (int r = 0; r < 3; r++) e(fcvt_sd(r, r));
      e(xo == 29 || xo == 31 ? fmadds(0, 0, 1, 2) : fnmsubs(0, 0, 1, 2));
      e(fcvt_ds(0, 0));
      if (xo >= 30) {  // negated, but a NaN is left as it is
        e(fcmpd(0, 0));
        e(fnegd(1, 0));
        e(fcselD(0, 0, 1, VS));
      }
      FW(fd, 0);
      rejoin();
      return false;
    }
    default:  // fres
      fallback(false);
      return false;
  }
  round_single(0);
  FW(fd, 0);
  return false;
}

bool Translator::op63(u32 insn) {  // double precision arithmetic, and the rest
  const int fd = (insn >> 21) & 31, fa = (insn >> 16) & 31, fb = (insn >> 11) & 31, fc = (insn >> 6) & 31;
  if (insn & 1) {
    fallback(false);
    return false;
  }
  switch ((insn >> 1) & 31) {
    case 18: FR(fa, 0); FR(fb, 1); e(fdivd(0, 0, 1)); FW(fd, 0); return false;  // fdiv
    case 20: FR(fa, 0); FR(fb, 1); e(fsubd(0, 0, 1)); FW(fd, 0); return false;  // fsub
    case 21: FR(fa, 0); FR(fb, 1); e(faddd(0, 0, 1)); FW(fd, 0); return false;  // fadd
    case 22: FR(fb, 0); e(fsqrtd(0, 0)); FW(fd, 0); return false;               // fsqrt
    case 23:  // fsel: fC if fA >= 0, else fB
      FR(fa, 0);
      FR(fc, 1);
      FR(fb, 2);
      e(fcmpd_zero(0));
      e(fcselD(0, 1, 2, GE));
      FW(fd, 0);
      return false;
    case 25: FR(fa, 0); FR(fc, 1); e(fmuld(0, 0, 1)); FW(fd, 0); return false;  // fmul
    case 29:  // fmadd: fA * fC + fB, rounded once
      FR(fa, 0);
      FR(fc, 1);
      FR(fb, 2);
      e(fmaddd(0, 0, 1, 2));
      FW(fd, 0);
      return false;
    case 28:  // fmsub: fA * fC - fB
      FR(fa, 0);
      FR(fc, 1);
      FR(fb, 2);
      e(fnmsubd(0, 0, 1, 2));
      FW(fd, 0);
      return false;
    case 31:    // fnmadd
    case 30: {  // fnmsub: the same two, negated, but a NaN is left as it is
      FR(fa, 0);
      FR(fc, 1);
      FR(fb, 2);
      e(((insn >> 1) & 31) == 31 ? fmaddd(0, 0, 1, 2) : fnmsubd(0, 0, 1, 2));
      e(fcmpd(0, 0));
      e(fnegd(1, 0));
      e(fcselD(0, 0, 1, VS));
      FW(fd, 0);
      return false;
    }
    case 26:  // frsqrte
      fallback(false);
      return false;
    default:
      break;
  }
  switch ((insn >> 1) & 0x3FF) {
    case 0:
    case 32: {  // fcmpu, fcmpo
      const u32 crfd = (insn >> 23) & 7;
      FR(fa, 0);
      FR(fb, 1);
      e(fcmpd(0, 1));
      e(movzw(S0, 1));  // unordered, unless one of these holds
      e(movzw(S1, 2));
      e(cselw(S0, S1, S0, EQ));
      e(movzw(S1, 4));
      e(cselw(S0, S1, S0, GT));
      e(movzw(S1, 8));
      e(cselw(S0, S1, S0, MI));
      e(bfiw(CRREG, S0, 28 - 4 * crfd, 4));
      load_state(S1, OFF(fpscr));
      e(bfiw(S1, S0, 12, 4));
      store_state(S1, OFF(fpscr));
      return false;
    }
    case 12:  // frsp
      FR(fb, 0);
      round_single(0);
      FW(fd, 0);
      return false;
    case 15:  // fctiwz: to an integer towards zero; a NaN gives the most negative
      FR(fb, 0);
      e(fcvtzs_wd(S0, 0));
      e(fcmpd(0, 0));
      e(movzw(S1, 0x8000, 1));
      e(cselw(S0, S1, S0, VS));
      e(movzx(S1, 0xFFF8, 3));
      e(orrx(S0, S1, S0));
      e(strx_at(S0, ST, fpr_at((u32)fd)));
      return false;
    case 40: FR(fb, 0); e(fnegd(0, 0)); FW(fd, 0); return false;                    // fneg
    case 72: FR(fb, 0); FW(fd, 0); return false;                                    // fmr
    case 136: FR(fb, 0); e(fabsd(0, 0)); e(fnegd(0, 0)); FW(fd, 0); return false;   // fnabs
    case 264: FR(fb, 0); e(fabsd(0, 0)); FW(fd, 0); return false;                   // fabs
    case 583:  // mffs
      load_state(S0, OFF(fpscr));
      e(movzx(S1, 0xFFF8, 3));
      e(orrx(S0, S1, S0));
      e(strx_at(S0, ST, fpr_at((u32)fd)));
      return false;
    case 711: {  // mtfsf
      const u32 fm = (insn >> 17) & 0xFF;
      u32 mask = 0;
      for (int i = 0; i < 8; i++) {
        if (fm & (1u << i)) mask |= 0xFu << (4 * i);
      }
      if (!mask) return false;
      load_state(S0, OFF(fpscr));
      e(ldrw_at(T0, ST, fpr_at((u32)fb)));
      e(eorw(T1, S0, T0));
      if (mask & 3) {
        // Changing the rounding mode also changes host thread state.
        e(tstw_i(T1, mask32(3)));
        rarely(forward(bcond(NE, 0), 19));
      }
      if (mask != 0xFFFFFFFFu) {
        imm(S1, mask);
        e(andw(T1, T1, S1));
      }
      e(eorw(S0, S0, T1));
      store_state(S0, OFF(fpscr));
      rejoin();
      return false;
    }
    default:  // conversions that round, and other FPSCR instructions
      fallback(false);
      return false;
  }
}

// ---- keeping blocks ----------------------------------------------------------

void point_site(u32 index, u32 to) {  // with the lock held and writing allowed
  u32* const at = (u32*)(g.base + g.sites[index].patch);
  *at = b((s64)to - (s64)g.sites[index].patch);
  sys_icache_invalidate(at, 4);
}

// The block for a guest address, translating it if need be. With the lock held.
Block* block_for(const u8* mem, u32 pc) {
  if (!start()) return nullptr;
  if (const auto it = g.by_pc.find(pc); it != g.by_pc.end()) return &g.blocks[it->second];
  if (g.used + (1u << 16) > kRegionSize) return nullptr;

  const u32 index = (u32)g.blocks.size();
  g.blocks.emplace_back();
  Block& block = g.blocks.back();
  const size_t sites_before = g.sites.size();
  bool fits;
  {
    Writing w;
    Translator t(mem, pc);
    fits = t.run(block);
    if (fits) sys_icache_invalidate(g.base + block.code, block.code_end - block.code);
  }
  if (!fits) {
    g.sites.resize(sites_before);
    g.blocks.pop_back();
    return nullptr;
  }
  g.used = (block.code_end + 15) & ~15u;
  g.by_pc[pc] = index;
  g.by_code[block.code] = index;
  const u32 last = pc + 4 * block.count - 1;
  for (u32 page = pc >> 12; page <= last >> 12; page++) g.by_page[page].push_back(index);
  return &block;
}

inline u64& table_entry(u32 pc) { return g.table[(pc >> 2) & ((1u << kTableBits) - 1)]; }

u8* code_for(const PPCState& s) {
  const u32 pc = s.pc;
  if (g.table) {
    const u64 entry = __atomic_load_n(&table_entry(pc), __ATOMIC_RELAXED);
    if ((u32)(entry >> 32) == pc && (u32)entry) return g.base + (u32)entry;
  }
  std::lock_guard<std::mutex> lk(g.mu);
  const Block* block = block_for(s.mem, pc);
  if (!block) return nullptr;
  __atomic_store_n(&table_entry(pc), (u64)pc << 32 | block->code, __ATOMIC_RELAXED);
  return g.base + block->code;
}

// A jump out of a block has been taken for the first time: make it direct.
void link(const PPCState& s, u32 site) {
  std::lock_guard<std::mutex> lk(g.mu);
  if (site >= g.sites.size()) return;
  Block* block = block_for(s.mem, g.sites[site].target);
  if (!block) return;
  Writing w;
  point_site(site, block->code);
  block->incoming.push_back(site);
}

bool enabled() {
  static const bool on = [] {
    const char* stats = getenv("COCAINE_JIT_STATS");
    g_stats.on = stats != nullptr;
    // A number of seconds: report then too, for programs that don't end.
    if (stats && atoi(stats) > 1) {
      const int secs = atoi(stats);
      std::thread([secs] {
        sleep((unsigned)secs);
        ppc_jit_report();
      }).detach();
    }
    const char* e = getenv("COCAINE_JIT");
    return !e || atoi(e) != 0;
  }();
  return on;
}

}  // namespace

void ppc_jit_invalidate(u32 addr, u64 size) {
  if (!g.base || !size) return;
  std::lock_guard<std::mutex> lk(g.mu);
  const u64 end = (u64)addr + size - 1;
  Writing w;
  for (u64 page = addr >> 12; page <= end >> 12 && page < (1u << 20); page++) {
    const auto it = g.by_page.find((u32)page);
    if (it == g.by_page.end()) continue;
    for (const u32 index : it->second) {
      Block& block = g.blocks[index];
      if (block.dead) continue;
      block.dead = true;
      if (const auto at = g.by_pc.find(block.pc); at != g.by_pc.end() && at->second == index) {
        g.by_pc.erase(at);
      }
      u64& entry = table_entry(block.pc);
      if ((u32)(entry >> 32) == block.pc && (u32)entry == block.code) {
        __atomic_store_n(&entry, 0, __ATOMIC_RELAXED);
      }
      for (const u32 site : block.incoming) point_site(site, g.sites[site].stub);
      block.incoming.clear();
    }
    g.by_page.erase(it);
  }
}

bool ppc_jit_fault(const void* context, PPCState& s) {
  const auto* uc = (const ucontext_t*)context;
  if (!uc || !uc->uc_mcontext || !g.base) return false;
  const auto& ss = uc->uc_mcontext->__ss;
  const u8* at = (const u8*)__darwin_arm_thread_state64_get_pc(ss);
  if (at < g.base || at >= g.base + kRegionSize) return false;
  if (!g.mu.try_lock()) return false;  // this is a fault handler: never wait
  bool found = false;
  const u32 offset = (u32)(at - g.base);
  auto it = g.by_code.upper_bound(offset);
  if (it != g.by_code.begin()) {
    const Block& block = g.blocks[(--it)->second];
    if (offset < block.code_end) {
      u32 n = 0;
      while (n + 1 < block.at.size() && block.at[n + 1] <= offset) n++;
      s.pc = block.pc + 4 * n;
      // The registers that were in host registers, as they stood.
      for (u32 r = 0; r < 32; r++) {
        if (kMap[r] >= 0) s.gpr[r] = (u32)ss.__x[kMap[r]];
      }
      s.cr = (u32)ss.__x[CRREG];
      found = true;
    }
  }
  g.mu.unlock();
  return found;
}

void ppc_jit_fork_lock(bool take) {
  if (take) {
    g.mu.lock();
    return;
  }
  g.mu.unlock();
  // A forked child's one thread can start out allowed to write translated
  // code and so not to run it, although its parent was the other way round
  // when it forked: seen on macOS 15, whenever the parent thread had last
  // been switched out by the kernel while it was translating. Say again
  // which it is.
  pthread_jit_write_protect_np(1);
}

void ppc_jit_report() {
  if (!g_stats.on) return;
  std::lock_guard<std::mutex> lk(g.mu);
  u64 guest = 0;
  for (const Block& b : g.blocks) guest += b.count;
  std::fprintf(stderr,
               "translator: %zu blocks, %llu guest instructions into %u bytes; left translated code %llu times "
               "to look a block up, %llu for the countdown, %llu to link, %llu for system calls, %llu for host "
               "calls, %llu stopped\n",
               g.blocks.size(), (unsigned long long)guest, g.used, (unsigned long long)g_stats.reasons[kLookup],
               (unsigned long long)g_stats.reasons[kBudget], (unsigned long long)g_stats.reasons[kLink],
               (unsigned long long)g_stats.reasons[kSyscall], (unsigned long long)g_stats.reasons[kHostCall],
               (unsigned long long)g_stats.reasons[kStop]);
  std::vector<std::pair<u64, u32>> order;
  {
    std::lock_guard<std::mutex> slk(g_stats.mu);
    for (const auto& e : g_stats.stepped) order.push_back({e.second, e.first});
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 40; i++) {
    std::fprintf(stderr, "translator:   %10llu by the interpreter: opcode %u.%u\n",
                 (unsigned long long)order[i].first, order[i].second >> 16, order[i].second & 0xFFFF);
  }
}

PPCStop ppc_run(PPCState& s, u64 max_insns) {
  if (!enabled() || ppc_debugging(s)) return ppc_interp(s, max_insns);

  ppc_apply_rounding(s.fpscr);
  pthread_jit_write_protect_np(1);  // see ppc_jit_fork_lock; it costs nothing to be sure
  s64 budget = (s64)max_insns;
  auto done = [&](PPCStopKind kind, u32 insn) {
    s.icount += max_insns - (u64)(budget > 0 ? budget : 0);
    host_rounding(FE_TONEAREST);  // host code expects the default mode
    return PPCStop{kind, s.pc, insn};
  };
  for (;;) {
    if (budget <= 0) return done(PPCStopKind::Budget, 0);
    u8* const code = code_for(s);
    if (!code) {
      // No room for more translations (or none to be had): interpret a while.
      const u64 n = budget < 4096 ? (u64)budget : 4096;
      const u64 before = s.icount;
      const PPCStop st = ppc_interp(s, n);
      budget -= (s64)(s.icount - before);
      s.icount = before;
      if (st.kind != PPCStopKind::Budget) {
        s.icount += max_insns - (u64)(budget > 0 ? budget : 0);
        return st;
      }
      ppc_apply_rounding(s.fpscr);
      continue;
    }
    const Ret r = ((Enter)(g.base + g.t_enter))(&s, s.mem, code, budget, g.table, g.base);
    budget = r.left;
    if (__builtin_expect(g_stats.on, 0)) __atomic_add_fetch(&g_stats.reasons[r.reason], 1, __ATOMIC_RELAXED);
    switch ((Reason)r.reason) {
      case kLookup:
        break;
      case kBudget:
        return done(PPCStopKind::Budget, 0);
      case kLink:
        link(s, s.jit_arg);
        break;
      case kSyscall:
        return done(PPCStopKind::Syscall, s.jit_arg);
      case kHostCall:
        return done(PPCStopKind::HostCall, s.jit_arg);
      case kStop: {
        const PPCStop st = tl_stop;
        s.icount += max_insns - (u64)(budget > 0 ? budget : 0);
        host_rounding(FE_TONEAREST);
        return st;
      }
    }
  }
}
