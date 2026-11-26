// The Intel translator: 32-bit x86 code turned into ARM64 code a block at a
// time. It is built like the PowerPC one (src/cpu/ppc/ppc_jit.cpp, which
// explains blocks, the jumps between them and the table), with these
// differences.
//
// All eight guest registers live in host registers that calls preserve, so
// the translator can call small routines without putting them away first.
// That is how it handles the hard part of x86, the flags: when the flags an
// instruction leaves are going to be read, the arithmetic is done by calling
// the interpreter's own routine for it, which sets eflags exactly as the
// interpreter would; when they are about to be overwritten unread, which is
// most of the time, the instruction is one or two host instructions and the
// flags are not computed at all. eflags itself stays in the X86State.
//
// Only the common integer instructions are translated. Anything else
// (floating point, SSE, string moves, 16-bit operands, the rare and the
// privileged) ends the block and is done by the interpreter, one
// instruction, before translated code carries on.
//
// x86 programs change their own code without saying so: the system's stubs
// for calling into other libraries are rewritten in place the first time
// each is used. Code in memory the guest can write is therefore translated
// one instruction at a time, and each such block first checks that the bytes
// it was made from are still there.

#include "cpu/rounding.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cpu/ppc/a64.h"
#include "cpu/x86/x86.h"

bool (*x86_jit_writable)(u32 addr) = nullptr;
bool (*x86_jit_readable)(u32 addr) = nullptr;

namespace {

using namespace a64;

constexpr u32 kRegionSize = 128u << 20;
constexpr u32 kTableBits = 16;
constexpr u32 kMaxBlock = 32;  // guest instructions

constexpr int ST = 19;   // the X86State
constexpr int MEM = 20;  // host address of guest address 0
constexpr int G(int r) { return 21 + r; }  // guest register r: eax ecx edx ebx esp ebp esi edi
constexpr int T0 = 12, T1 = 13, T2 = 14, S0 = 16, S1 = 17;  // scratch
constexpr int kEsp = 4, kEbp = 5;

enum Reason : u32 { kLookup, kBudget, kLink, kStop, kStale };

#define XOFF(field) ((u32)__builtin_offsetof(X86State, field))

struct Site {
  u32 patch, stub, target;
};
struct Block {
  u32 pc = 0, bytes = 0, code = 0, code_end = 0;
  bool dead = false;
  std::vector<u32> incoming;
  std::vector<std::pair<u32, u32>> at;  // code offset, guest address, per instruction
};
struct Jit {
  std::mutex mu;
  bool tried = false;
  u8* base = nullptr;
  u32 used = 0;
  u64* table = nullptr;
  std::deque<Block> blocks;
  std::vector<Site> sites;
  std::unordered_map<u32, u32> by_pc;
  std::map<u32, u32> by_code;
  std::unordered_map<u32, std::vector<u32>> by_page;
  u32 t_enter = 0, t_exit = 0, t_lookup = 0, t_lookup_kept = 0, t_budget = 0, t_link = 0, t_stop = 0,
      t_stale = 0, t_step = 0, t_alu = 0, t_incdec = 0, t_shift = 0, t_x87 = 0, t_flush = 0, t_verify = 0,
      t_poison = 0;
};
Jit g;
thread_local X86Stop tl_stop;

struct Writing {
  Writing() { pthread_jit_write_protect_np(0); }
  ~Writing() { pthread_jit_write_protect_np(1); }
};

// One instruction, done by the interpreter; nonzero if it stopped for the
// kernel layer instead.
// For working on the translator: with COCAINE_JIT_STATS set, counts which
// instructions went to the interpreter, by their first bytes.
bool g_counting = false;
bool g_maths = true;  // COCAINE_JIT_MATHS=0: floating point and vectors go to the interpreter
std::mutex g_count_mu;
std::unordered_map<u32, u64> g_stepped;

// Flags that are waiting. An arithmetic instruction sets six flags that
// nearly nobody reads, so translated code only notes what the instruction
// was and what it was given (lazy_kind, lazy_a, lazy_b in the X86State),
// and the flags are worked out from the note if somebody does want them.
// The low byte of the note is one more than the operation's number among
// the eight, or kLazyInc and up for inc and dec; the next byte is the
// operand size in bits. Everything outside translated code calls this
// first, so only translated code ever sees a note.
constexpr u32 kLazyInc = 16;
constexpr u32 kPoison = 0xFF;  // see COCAINE_JIT_POISON
inline void flush_flags(X86State* s) {
  const u32 note = s->lazy_kind;
  if (!note) return;
  s->lazy_kind = 0;
  if (note == kPoison) return;
  const u32 code = note & 0xFF, bits = note >> 8;
  if (code >= kLazyInc) {
    x86_incdec(s, s->lazy_a, code - kLazyInc, bits);
  } else {
    x86_alu(s, code - 1, s->lazy_a, s->lazy_b, bits);
  }
}
u32 helper_flush(X86State* s) {
  flush_flags(s);
  return 0;
}
// The interpreter's arithmetic, for translated code: the ones that read or
// keep some of the old flags need them worked out first.
u32 helper_alu(X86State* s, u32 op, u32 a, u32 b, u32 bits) {
  flush_flags(s);
  return x86_alu(s, op, a, b, bits);
}
u32 helper_incdec(X86State* s, u32 a, u32 dec, u32 bits) {
  flush_flags(s);
  return x86_incdec(s, a, dec, bits);
}
u32 helper_shift(X86State* s, u32 kind, u32 a, u32 count, u32 bits) {
  flush_flags(s);
  return x86_shift(s, kind, a, count, bits);
}

// COCAINE_JIT_VERIFY=1, for working on cocaine: each translated SSE
// instruction is also run by the interpreter, on a copy of the state, and
// the registers compared afterwards. The first few differences are reported.
bool g_verify = false;
bool g_barriers = false;  // COCAINE_JIT_BARRIERS=1
bool g_writable_interpreted = false;
// COCAINE_JIT_FLAGS: 3 (the usual) skips flags nobody reads (1) and fuses
// compares with the branches after them (2); 0 does neither.
int g_flags_mode = 3;
// COCAINE_JIT_POISON=1, for working on cocaine: flags the translator decided
// nobody reads are marked, and a conditional jump, set or move that reads
// them anyway is reported, with where they came from.
bool g_poison = false;
u32 helper_poisoned(X86State* s, u32 reader) {
  static std::mutex mu;
  static std::vector<std::pair<u32, u32>> told;
  std::lock_guard<std::mutex> lk(mu);
  const std::pair<u32, u32> pair{s->lazy_a, reader};
  for (const auto& t : told) {
    if (t == pair) return 0;
  }
  if (told.size() < 40) {
    told.push_back(pair);
    const bool interp = reader & 0x80000000u;
    reader &= 0x7FFFFFFFu;
    const u8* a = s->mem + s->lazy_a;
    const u8* b = s->mem + reader;
    std::fprintf(stderr, "cocaine: flags of 0x%08x (%02x %02x %02x %02x), skipped as unread, %s at 0x%08x (%02x %02x %02x %02x %02x %02x)\n",
                 s->lazy_a, a[0], a[1], a[2], a[3], interp ? "reach the interpreter" : "read", reader, b[0], b[1],
                 b[2], b[3], b[4], b[5]);
  }
  return 0;
}  // COCAINE_JIT_WRITABLE=0: code in writable memory is interpreted
thread_local X86State t_expected;
thread_local u32 t_verify_pc = 0;
u32 helper_verify(X86State* s, u32 arg) {
  if (!(arg & 0x80000000u)) {  // before: what the interpreter makes of it
    t_expected = *s;
    t_expected.eip = arg;
    t_expected.trace = false;
    x86_interp(t_expected, 1);
    sse_apply_rounding(s->mxcsr);
    t_verify_pc = arg;
    return 0;
  }
  if (t_verify_pc != (arg & 0x7FFFFFFFu)) return 0;
  t_verify_pc = 0;
  const bool xmm_same = memcmp(t_expected.xmm, s->xmm, sizeof s->xmm) == 0;
  const bool regs_same = memcmp(t_expected.reg, s->reg, sizeof s->reg) == 0;
  if (xmm_same && regs_same) return 0;
  static std::atomic<int> told{0};
  if (told++ >= 8) return 0;
  const u32 pc = arg & 0x7FFFFFFFu;
  const u8* b = s->mem + pc;
  std::fprintf(stderr, "cocaine: translated SSE differs from the interpreter at 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               pc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
  for (int r = 0; r < 8; r++) {
    if (memcmp(&t_expected.xmm[r], &s->xmm[r], 16) != 0) {
      std::fprintf(stderr, "  xmm%d: interpreter %016llx%016llx, translated %016llx%016llx\n", r,
                   (unsigned long long)t_expected.xmm[r].q[1], (unsigned long long)t_expected.xmm[r].q[0],
                   (unsigned long long)s->xmm[r].q[1], (unsigned long long)s->xmm[r].q[0]);
    }
    if (t_expected.reg[r] != s->reg[r]) {
      std::fprintf(stderr, "  r%d: interpreter %08x, translated %08x\n", r, t_expected.reg[r], s->reg[r]);
    }
  }
  return 0;
}

u32 helper_step(X86State* s, u32 eip) {
  s->eip = eip;
  if (__builtin_expect(g_poison, 0) && s->lazy_kind == kPoison) helper_poisoned(s, eip | 0x80000000u);
  flush_flags(s);
  if (__builtin_expect(g_counting, 0)) {
    const u8* p = s->mem + eip;
    u32 key = 0;
    int n = 0;
    for (int i = 0; i < 6 && n < 3; i++) {  // up to three bytes, the prefixes among them
      key = key << 8 | p[i];
      n++;
      if (p[i] != 0x66 && p[i] != 0xF2 && p[i] != 0xF3 && p[i] != 0x0F && p[i] != 0x65) break;
    }
    std::lock_guard<std::mutex> lk(g_count_mu);
    g_stepped[key]++;
  }
  t_keep_guest_rounding = true;
  const X86Stop st = x86_interp(*s, 1);
  t_keep_guest_rounding = false;
  sse_apply_rounding(s->mxcsr);  // (free: the mode is already the guest's)
  if (st.kind == X86StopKind::Budget) return 0;
  tl_stop = st;
  return 1;
}

// One x87 instruction, for translated code that carries on afterwards. The
// answer is 0 for one the interpreter doesn't know.
u32 helper_x87(X86State* s, u32 op, u32 modrm, u32 ea) {
  flush_flags(s);  // some of them read the flags, some set three
  return x87_exec(*s, (u8)op, (u8)modrm, ea) ? 1 : 0;
}

struct Emitter {
  u32* p = nullptr;
  void e(u32 word) { *p++ = word; }
  u32 here() const { return (u32)((u8*)p - g.base); }
  void jump(u32 to) { e(b((s64)to - (s64)here())); }
  void call(u32 to) { e(bl((s64)to - (s64)here())); }
  void imm(int rd, u32 v) {
    if ((v & 0xFFFF0000u) == 0) {
      e(movzw(rd, v));
    } else if ((v & 0xFFFFu) == 0) {
      e(movzw(rd, v >> 16, 1));
    } else if ((v & 0xFFFF0000u) == 0xFFFF0000u) {
      e(movnw(rd, ~v & 0xFFFF));
    } else {
      e(movzw(rd, v & 0xFFFF));
      e(movkw(rd, v >> 16, 1));
    }
  }
  void imm64(int rd, u64 a) {
    e(movzx(rd, a & 0xFFFF));
    e(movkx(rd, (a >> 16) & 0xFFFF, 1));
    e(movkx(rd, (a >> 32) & 0xFFFF, 2));
    e(movkx(rd, (a >> 48) & 0xFFFF, 3));
  }
  void spill() {
    for (int r = 0; r < 8; r++) e(strw_at(G(r), ST, XOFF(reg) + 4 * (u32)r));
  }
  void fill() {
    for (int r = 0; r < 8; r++) e(ldrw_at(G(r), ST, XOFF(reg) + 4 * (u32)r));
  }
};

void emit_common() {
  Emitter m;
  m.p = (u32*)g.base;
  u32* const zero = m.p;  // where an empty table entry leads: guest address 0
  m.e(0);

  g.t_exit = m.here();  // w0: the reason
  m.e(ldp_at(19, 20, SP, 16));
  m.e(ldp_at(21, 22, SP, 32));
  m.e(ldp_at(23, 24, SP, 48));
  m.e(ldp_at(25, 26, SP, 64));
  m.e(ldp_at(27, 28, SP, 80));
  m.e(ldp_post(29, 30, SP, 96));
  m.e(ret());

  g.t_enter = m.here();  // x0 state, x1 code
  m.e(stp_pre(29, 30, SP, -96));
  m.e(stp_at(19, 20, SP, 16));
  m.e(stp_at(21, 22, SP, 32));
  m.e(stp_at(23, 24, SP, 48));
  m.e(stp_at(25, 26, SP, 64));
  m.e(stp_at(27, 28, SP, 80));
  m.e(movx(ST, 0));
  m.e(ldrx_at(MEM, ST, XOFF(mem)));
  m.e(movx(S0, 1));
  m.fill();
  m.e(br(S0));

  // Ways out: w16 is the guest address to go on from, w17 a word to pass.
  auto leave = [&](bool keep_eip, bool arg, Reason why) {
    const u32 at = m.here();
    m.spill();
    if (!keep_eip) m.e(strw_at(S0, ST, XOFF(eip)));
    if (arg) m.e(strw_at(S1, ST, XOFF(jit_arg)));
    m.e(movzw(0, why));
    m.jump(g.t_exit);
    return at;
  };
  g.t_lookup = leave(false, false, kLookup);
  g.t_lookup_kept = leave(true, false, kLookup);
  g.t_budget = leave(false, false, kBudget);
  g.t_link = leave(false, true, kLink);
  g.t_stop = leave(true, false, kStop);
  g.t_stale = leave(false, false, kStale);

  // The interpreter for one instruction at w17: it wants the registers in
  // the X86State, and may change any of them. Its answer comes back in w16.
  g.t_step = m.here();
  m.spill();
  m.e(stp_pre(29, 30, SP, -16));
  m.e(movx(0, ST));
  m.e(movw(1, S1));
  m.imm64(S0, (u64)(const void*)&helper_step);
  m.e(blr(S0));
  m.e(ldp_post(29, 30, SP, 16));
  m.e(movw(S0, 0));
  m.fill();
  m.e(ret());

  // The SSE checker (COCAINE_JIT_VERIFY), which wants the registers in the
  // X86State and changes none of them.
  g.t_verify = m.here();
  m.spill();
  m.e(stp_pre(29, 30, SP, -16));
  m.e(movx(0, ST));
  m.e(movw(1, S1));
  m.imm64(S0, (u64)(const void*)&helper_verify);
  m.e(blr(S0));
  m.e(ldp_post(29, 30, SP, 16));
  m.fill();
  m.e(ret());

  // The interpreter's arithmetic: plain calls, arguments already in place.
  auto leaf = [&](const void* routine) {
    const u32 at = m.here();
    m.imm64(S0, (u64)routine);
    m.e(br(S0));
    return at;
  };
  g.t_alu = leaf((const void*)&helper_alu);
  g.t_incdec = leaf((const void*)&helper_incdec);
  g.t_shift = leaf((const void*)&helper_shift);
  g.t_flush = leaf((const void*)&helper_flush);
  g.t_poison = leaf((const void*)&helper_poisoned);
  g.t_x87 = leaf((const void*)&helper_x87);

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

// ---- decoding ----------------------------------------------------------------

enum Kind {
  kAluRmR, kAluRRm, kAluAImm, kGroup1, kIncDecReg, kPushReg, kPopReg, kPushImm, kJcc, kTestRmR, kTestAImm,
  kMovRmR, kMovRRm, kLea, kMovRImm, kMovRmImm, kMovAMem, kMovMemA, kShift, kRet, kCall, kJmp, kGroup5,
  kGroup3, kLeave, kNop, kCdq, kMovX, kImul2, kImul3, kSetcc, kCmov, kSse, kX87, kBitScan,
};

struct Ins {
  u32 len = 0;
  int kind = 0, sz = 4;
  u8 op = 0;
  bool gs = false;
  u8 mod = 3, reg = 0, rm = 0, scale = 0, index = 4, base = 0;
  bool has_base = false;
  u32 disp = 0, imm = 0;
  u8 ib = 0;  // an SSE instruction's trailing byte
  bool uses = false;  // reads the flags
  bool defs = false;  // sets every arithmetic flag
  bool ends = false;  // control leaves the block here, always
};

// The SSE instructions translated, by prefix and second opcode byte: moves,
// arithmetic and compares, conversions, the bitwise ones, and the plainest
// of the integer ones. The rest (and everything MMX) go to the interpreter.
// COCAINE_JIT_SKIP=70,62: SSE instructions (by second opcode byte) left to
// the interpreter, for finding a translation that's wrong.
bool g_skip[256];
bool sse_known(u8 prefix, u8 op) {
  if (!g_maths || g_skip[op]) return false;
  switch (op) {
    case 0x10: case 0x11: return true;                               // movups movss movupd movsd
    case 0x12: case 0x13: return prefix == 0 || prefix == 0x66;      // movlps movlpd, movhlps
    case 0x14: case 0x15: return prefix == 0 || prefix == 0x66;      // unpcklps unpckhps unpcklpd unpckhpd
    case 0x16: case 0x17: return prefix == 0 || prefix == 0x66;      // movhps movhpd, movlhps
    case 0x28: case 0x29: return prefix == 0 || prefix == 0x66;      // movaps movapd
    case 0x6F: case 0x7F: return prefix == 0x66 || prefix == 0xF3;   // movdqa movdqu
    case 0x2A: return prefix == 0xF2 || prefix == 0xF3;              // cvtsi2sd cvtsi2ss
    case 0x2C: case 0x2D: return prefix == 0xF2 || prefix == 0xF3;   // cvttsd2si cvttss2si cvtsd2si cvtss2si
    case 0x2E: case 0x2F: return prefix == 0 || prefix == 0x66;      // ucomiss comiss ucomisd comisd
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5E:           // sqrt add mul sub div
    case 0x5D: case 0x5F:                                            // min max
      return true;                                                   // one, or all of them at once
    case 0x5A: return true;                                          // cvtsd2ss cvtss2sd cvtps2pd cvtpd2ps
    case 0x5B: return prefix != 0xF2;                                // cvtdq2ps cvtps2dq cvttps2dq
    case 0xE6: return prefix == 0xF3;                                // cvtdq2pd
    case 0xC2: return true;                                          // compares that leave a mask
    case 0xC6: return prefix == 0 || prefix == 0x66;                 // shufps shufpd
    case 0x70: return prefix == 0x66;                                // pshufd
    case 0x71: case 0x72: case 0x73: return prefix == 0x66;          // shifts by a constant
    case 0x60: case 0x61: case 0x62: case 0x6C:                      // punpcklbw wd dq qdq
    case 0x68: case 0x69: case 0x6A: case 0x6D:                      // punpckhbw wd dq qdq
    case 0xD5:                                                       // pmullw
      return prefix == 0x66;
    case 0x54: case 0x55: case 0x56: case 0x57:                      // and andn or xor
      return prefix == 0 || prefix == 0x66;
    case 0x6E: return prefix == 0x66;                                // movd, in
    case 0x7E: return prefix == 0x66 || prefix == 0xF3;              // movd, out; movq, in
    case 0xD6: return prefix == 0x66;                                // movq, out
    case 0xD7: return prefix == 0x66;                                // pmovmskb
    case 0xDB: case 0xDF: case 0xEB: case 0xEF:                      // pand pandn por pxor
    case 0xFC: case 0xFD: case 0xFE: case 0xD4:                      // paddb paddw paddd paddq
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:                      // psubb psubw psubd psubq
    case 0x74: case 0x75: case 0x76:                                 // pcmpeqb pcmpeqw pcmpeqd
    case 0x64: case 0x65: case 0x66:                                 // pcmpgtb pcmpgtw pcmpgtd
      return prefix == 0x66;
    default: return false;
  }
}

// Decodes the instruction at `pc` if it is one the translator handles.
bool decode(const u8* mem, u32 pc, Ins& n) {
  const u8* p = mem + pc;
  u32 i = 0;
  auto rd32 = [&] {
    u32 v;
    memcpy(&v, p + i, 4);
    i += 4;
    return v;
  };
  auto modrm = [&] {
    const u8 b = p[i++];
    n.mod = b >> 6;
    n.reg = (b >> 3) & 7;
    n.rm = b & 7;
    if (n.mod == 3) return;
    if (n.rm == 4) {
      const u8 sib = p[i++];
      n.scale = sib >> 6;
      n.index = (sib >> 3) & 7;
      n.base = sib & 7;
      n.has_base = !(n.base == 5 && n.mod == 0);
      if (!n.has_base) n.disp = rd32();
    } else if (n.rm == 5 && n.mod == 0) {
      n.disp = rd32();
    } else {
      n.has_base = true;
      n.base = n.rm;
    }
    if (n.mod == 1) {
      n.disp += (u32)(s32)(s8)p[i++];
    } else if (n.mod == 2) {
      n.disp += rd32();
    }
  };
  if (p[i] == 0x65) {  // the thread's own data
    n.gs = true;
    i++;
  }
  {  // a nop with an operand, however many prefixes pad it out
    u32 j = i;
    while (p[j] == 0x66) j++;
    if (p[j] == 0x2E) j++;
    if (p[j] == 0x0F && p[j + 1] == 0x1F) {
      i = j + 2;
      modrm();
      n.kind = kNop;
      n.len = i;
      return true;
    }
    if (j == i + 1 && p[j] == 0x90) {
      n.kind = kNop;
      n.len = 2;
      return true;
    }
  }
  u8 prefix = 0;  // of an SSE instruction: 66, F2 or F3 before the 0F
  if ((p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3) && p[i + 1] == 0x0F) prefix = p[i++];
  const u8 op = n.op = p[i++];
  if (op == 0x0F && sse_known(prefix, p[i])) {
    n.kind = kSse;
    n.op = p[i++];
    n.imm = prefix;
    modrm();
    if (n.mod == 3 && (n.op == 0x13 || n.op == 0x17 || ((n.op == 0x12 || n.op == 0x16) && prefix))) return false;
    if (n.op == 0xD7 && n.mod != 3) return false;
    if (n.op == 0xC2 || n.op == 0xC6 || n.op == 0x70) n.ib = p[i++];
    if (n.op == 0xC2 && n.ib > 7) return false;
    if (n.op >= 0x71 && n.op <= 0x73) {  // on the register the last three bits name
      n.ib = p[i++];
      if (n.mod != 3 || (n.reg != 2 && n.reg != 6 && !(n.reg == 4 && n.op != 0x73))) return false;
    }
    n.defs = n.op == 0x2E || n.op == 0x2F;  // a compare: sets them all
  } else if (prefix) {
    return false;
  } else if (op < 0x40 && (op & 7) <= 5) {  // the eight arithmetic operations
    const u32 form = op & 7, which = op >> 3;
    n.sz = (form & 1) ? 4 : 1;
    n.reg = (u8)which;  // overwritten by modrm for the first four forms; kept for the last two
    if (form < 4) {
      modrm();
      n.imm = which;
      n.kind = form < 2 ? kAluRmR : kAluRRm;
    } else {
      n.kind = kAluAImm;
      const u32 v = n.sz == 4 ? rd32() : p[i++];
      n.disp = v;
      n.imm = which;
    }
    n.uses = which == 2 || which == 3;
    n.defs = true;
  } else if (op >= 0x40 && op <= 0x4F) {
    n.kind = kIncDecReg;  // changes five flags and reads none: neither uses nor defs
  } else if (op >= 0x50 && op <= 0x57) {
    n.kind = kPushReg;
  } else if (op >= 0x58 && op <= 0x5F) {
    if ((op & 7) == kEsp) return false;
    n.kind = kPopReg;
  } else if (op == 0x68 || op == 0x6A) {
    n.kind = kPushImm;
    n.imm = op == 0x68 ? rd32() : (u32)(s32)(s8)p[i++];
  } else if (op == 0x69 || op == 0x6B) {
    n.kind = kImul3;
    modrm();
    n.imm = op == 0x69 ? rd32() : (u32)(s32)(s8)p[i++];
    // A multiply sets only the carry and the overflow; the interpreter
    // leaves the other four as they were, so it is not one that sets them
    // all: a compare before it must still be seen by a sete after it.
  } else if (op >= 0x70 && op <= 0x7F) {
    n.kind = kJcc;
    const s32 rel = (s8)p[i++];
    n.imm = pc + i + (u32)rel;
    n.uses = true;
  } else if (op == 0x80 || op == 0x81 || op == 0x83) {
    n.kind = kGroup1;
    n.sz = op == 0x80 ? 1 : 4;
    modrm();
    n.imm = op == 0x81 ? rd32() : op == 0x80 ? p[i++] : (u32)(s32)(s8)p[i++];
    n.uses = n.reg == 2 || n.reg == 3;
    n.defs = true;
  } else if (op == 0x84 || op == 0x85) {
    n.kind = kTestRmR;
    n.sz = op == 0x84 ? 1 : 4;
    modrm();
    n.defs = true;
  } else if (op >= 0x88 && op <= 0x8B) {
    n.kind = op < 0x8A ? kMovRmR : kMovRRm;
    n.sz = (op & 1) ? 4 : 1;
    modrm();
  } else if (op == 0x8D) {
    n.kind = kLea;
    modrm();
    if (n.mod == 3) return false;
  } else if (op == 0x90) {
    n.kind = kNop;
  } else if (op == 0x99) {
    n.kind = kCdq;
  } else if (op == 0xA1 || op == 0xA3) {
    n.kind = op == 0xA1 ? kMovAMem : kMovMemA;
    n.disp = rd32();
  } else if (op == 0xA8 || op == 0xA9) {
    n.kind = kTestAImm;
    n.sz = op == 0xA8 ? 1 : 4;
    n.imm = n.sz == 4 ? rd32() : p[i++];
    n.defs = true;
  } else if (op >= 0xB0 && op <= 0xBF) {
    n.kind = kMovRImm;
    n.sz = op >= 0xB8 ? 4 : 1;
    n.imm = n.sz == 4 ? rd32() : p[i++];
  } else if (op == 0xC1 || op == 0xD1 || op == 0xD3) {
    n.kind = kShift;
    modrm();
    n.imm = op == 0xC1 ? p[i++] & 31u : 1;
    const bool by_cl = op == 0xD3, plain = n.reg >= 4;
    // A count of zero changes nothing; rotates leave most flags alone; and
    // nobody knows what a count in cl will be.
    n.defs = plain && !by_cl && n.imm != 0;
    n.uses = n.reg == 2 || n.reg == 3;  // the two rotates through the carry read it
  } else if (op == 0xC2 || op == 0xC3) {
    n.kind = kRet;
    if (op == 0xC2) {
      n.imm = (u32)p[i] | (u32)p[i + 1] << 8;
      i += 2;
    }
    n.ends = true;
  } else if (op == 0xC6 || op == 0xC7) {
    n.kind = kMovRmImm;
    n.sz = op == 0xC7 ? 4 : 1;
    modrm();
    if (n.reg != 0) return false;
    n.imm = n.sz == 4 ? rd32() : p[i++];
  } else if (op == 0xC9) {
    n.kind = kLeave;
  } else if (op == 0xE8 || op == 0xE9 || op == 0xEB) {
    n.kind = op == 0xE8 ? kCall : kJmp;
    const s32 rel = op == 0xEB ? (s32)(s8)p[i++] : (s32)rd32();
    n.imm = pc + i + (u32)rel;
    n.ends = true;
  } else if (op >= 0xD8 && op <= 0xDF) {
    if (!g_maths) return false;
    n.kind = kX87;
    modrm();
    // The ones between registers include the conditional moves, which read
    // the flags, and the compares that set only three of them.
    n.uses = n.mod == 3 && (op == 0xDA || op == 0xDB || op == 0xDF);
  } else if (op == 0xF6 || op == 0xF7) {
    n.kind = kGroup3;
    n.sz = op == 0xF7 ? 4 : 1;
    modrm();
    if (n.reg == 0) {
      n.imm = n.sz == 4 ? rd32() : p[i++];
      n.defs = true;
    } else if (n.sz == 4 && n.reg == 2) {  // not
    } else if (n.sz == 4 && n.reg == 3) {  // neg
      n.defs = true;
    } else {
      return false;
    }
  } else if (op == 0xFF) {
    n.kind = kGroup5;
    modrm();
    if (n.reg == 0 || n.reg == 1) {
      // inc and dec: as above, neither uses nor defs
    } else if (n.reg == 2 || n.reg == 4) {
      n.ends = true;
    } else if (n.reg != 6) {
      return false;
    }
  } else if (op == 0x0F) {
    const u8 op2 = n.op = p[i++];
    if (op2 >= 0x80 && op2 <= 0x8F) {
      n.kind = kJcc;
      const s32 rel = (s32)rd32();
      n.imm = pc + i + (u32)rel;
      n.uses = true;
    } else if (op2 >= 0x90 && op2 <= 0x9F) {
      n.kind = kSetcc;
      modrm();
      n.uses = true;
    } else if (op2 >= 0x40 && op2 <= 0x4F) {
      n.kind = kCmov;
      modrm();
      n.uses = true;
    } else if (op2 == 0xAF) {
      n.kind = kImul2;
      modrm();  // carry and overflow only, as above: neither uses nor defs
    } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
      n.kind = kMovX;
      modrm();
    } else if (op2 == 0xBC || op2 == 0xBD) {
      n.kind = kBitScan;
      modrm();
      // Only the zero flag changes, and none is read: neither uses nor defs.
    } else {
      return false;
    }
  } else {
    return false;
  }
  n.len = i;
  return true;
}

// ---- translating one block ---------------------------------------------------

class Translator : Emitter {
 public:
  Translator(const u8* mem, u32 pc) : mem_(mem), pc_(pc) { p = (u32*)(g.base + g.used); }

  bool run(Block& block) {
    const u32 first = pc_;
    block.pc = first;
    block.code = here();
    e(ldrx_at(S0, ST, XOFF(jit_budget)));
    u32* const count = p;
    e(0);
    e(strx_at(S0, ST, XOFF(jit_budget)));
    cold_.push_back({kColdBudget, forward(bcond(MI, 0), 19), first, 0});

    // Code the guest could overwrite (Leopard's Intel graphics libraries
    // write drawing routines at run time, and reuse the memory for others):
    // one instruction, every byte of it checked on the way in, and nothing
    // assumed about what follows it (checked_ turns the flags look-ahead off).
    const bool checked = x86_jit_writable && x86_jit_writable(first);
    checked_ = checked;
    if (checked) {
      Ins whole;
      const u32 len = decode(mem_, first, whole) ? whole.len : 8;
      // Two overlapping words cover an instruction of up to 16 bytes.
      const u32 at[2] = {first, first + (len > 8 ? len - 8 : 0)};
      for (int k = 0; k < (len > 8 ? 2 : 1); k++) {
        u64 bytes;
        memcpy(&bytes, mem_ + at[k], 8);
        imm(S1, at[k]);
        e(ldrx_idx(S0, MEM, S1));
        imm64(S1, bytes);
        e(cmpx(S0, S1));
        cold_.push_back({kColdStale, forward(bcond(NE, 0), 19), first, 0});
      }
    }

    bool ended = false;
    u32 n = 0;
    const u32 limit = checked ? 1 : kMaxBlock;
    while (!ended && n < limit) {
      block.at.push_back({here(), pc_});
      Ins ins;
      n++;
      room_ = limit - n;
      keep_ = false;
      if (!decode(mem_, pc_, ins) || !one(ins)) {
        fallback();
        ended = true;
        break;
      }
      // ARM's flags go on meaning the guest's only through instructions
      // whose translation leaves them alone.
      if (!keep_ && !transparent(ins.kind)) native_ = kNoCls;
      if (g_barriers) e(0xD5033BBFu);  // dmb ish: an experiment in memory ordering
      pc_ += ins.len;
      ended = ins.ends;
    }
    if (!ended) site(pc_);
    *count = subsx_i(S0, S0, n);
    block.bytes = (checked ? 16 : 0) + (pc_ > first ? pc_ - first : 1) + 16;

    for (const Cold& c : cold_) {
      bind(c.fix);
      switch (c.kind) {
        case kColdBudget:
          imm(S0, c.a);
          jump(g.t_budget);
          break;
        case kColdStale:
          imm(S0, c.a);
          jump(g.t_stale);
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
          e(movw(S0, T0));
          jump(g.t_lookup);
          break;
        case kColdPoison:  // COCAINE_JIT_POISON: flags read that were thought unread
          e(movx(0, ST));
          imm(1, c.a);
          call(g.t_poison);
          e(b((s64)((u8*)c.back - (u8*)p)));
          break;
        case kColdFlush:  // flags were waiting, and are wanted
          e(movx(0, ST));
          call(g.t_flush);
          e(b((s64)((u8*)c.back - (u8*)p)));
          break;
        case kColdStep:  // this one instruction by the interpreter after all, then back
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
    int bits;
  };
  enum { kColdBudget, kColdStale, kColdSite, kColdStop, kColdMiss, kColdStep, kColdFlush, kColdPoison };
  struct Cold {
    int kind;
    Fix fix;
    u32 a, b;
    u32* back = nullptr;  // for kColdStep: where to carry on afterwards
  };
  const u8* mem_;
  u32 pc_;
  std::vector<Cold> cold_;

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

  void add_imm(int rd, int rn, u32 u) {
    const u32 neg = 0u - u;
    if (u == 0) {
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

  // ---- flags ----
  // A compare and the branch behind it are the commonest pair of
  // instructions there is, so they become ARM's own: a flag-setting ARM
  // instruction and a conditional branch on ARM's flags. native_ says what
  // ARM's flags hold just now, which decides how an x86 condition reads
  // them; it lasts through instructions whose translation leaves ARM's
  // flags alone. The guest's own flags are another matter: if nothing can
  // read them they are skipped, and otherwise a note is left for
  // flush_flags (above).
  enum { kNoCls, kSubCls, kAddCls, kLogicCls, kStepCls, kFloatCls };
  int native_ = kNoCls;
  bool keep_ = false;          // the instruction being translated leaves native_ true
  bool checked_ = false;       // translating code the guest can write
  bool known_clear_ = false;   // no note can be waiting at this point of the block
  u32 room_ = 0;               // how many more instructions this block will take
  std::vector<u32> promised_;  // conditions counted on to read ARM's flags

  // The ARM condition that x86 condition `cc` comes to, or -1 for none.
  // After a subtraction ARM's carry is x86's upside down; after and, or,
  // xor both carries and overflows are clear; inc and dec leave x86's carry
  // alone, so nothing that reads it; and a floating point compare has an
  // arrangement of its own.
  static int native_cond(int cls, u32 cc) {
    const bool sub = cls == kSubCls, add = cls == kAddCls, logic = cls == kLogicCls, fp = cls == kFloatCls;
    if (cls == kNoCls) return -1;
    switch (cc) {
      case 0: return fp ? -1 : VS;
      case 1: return fp ? -1 : VC;
      case 2: return sub ? LO : (add || logic) ? HS : fp ? LT : -1;
      case 3: return sub ? HS : (add || logic) ? LO : fp ? GE : -1;
      case 4: return fp ? -1 : EQ;
      case 5: return fp ? -1 : NE;
      case 6: return sub ? LS : logic ? EQ : fp ? LE : -1;
      case 7: return sub ? HI : logic ? NE : fp ? GT : -1;
      case 8: return fp ? -1 : MI;
      case 9: return fp ? -1 : PL;
      case 10: return fp ? VS : -1;
      case 11: return fp ? VC : -1;
      case 12: return fp ? -1 : LT;
      case 13: return fp ? -1 : GE;
      case 14: return fp ? -1 : LE;
      default: return fp ? -1 : GT;
    }
  }
  // Instructions that touch neither the guest's flags nor, translated, ARM's.
  static bool transparent(int kind) {
    switch (kind) {
      case kPushReg: case kPopReg: case kPushImm: case kMovRmR: case kMovRRm: case kLea: case kMovRImm:
      case kMovRmImm: case kMovAMem: case kMovMemA: case kLeave: case kNop: case kCdq: case kMovX:
        return true;
      default:
        return false;
    }
  }
  static bool reads_cc(int kind) { return kind == kJcc || kind == kSetcc || kind == kCmov; }
  static bool readable(u32 at) { return x86_jit_readable && x86_jit_readable(at) && x86_jit_readable(at + 255); }

  // Whether the flags, as they stand before the instruction at `at`, are
  // overwritten before anything can read them.
  bool dead_from(u32 at) {
    if (checked_ || !(g_flags_mode & 1)) return false;  // what follows may change before it runs
    if (g_flags_mode & 8) return true;  // deliberately wrong, to test COCAINE_JIT_POISON
    for (int k = 0; k < 16; k++) {
      Ins n;
      if (!decode(mem_, at, n) || n.uses) return false;
      if (n.defs) return true;
      if (g_flags_mode & 4) {  // the old, cautious rules: anything that touches the flags stops the look
        if (n.kind == kIncDecReg || n.kind == kShift || n.kind == kBitScan || n.kind == kX87 || n.kind == kJmp ||
            n.kind == kImul2 || n.kind == kImul3 ||
            (n.kind == kGroup5 && n.reg <= 1)) {
          return false;
        }
      }
      if (n.kind == kJmp) {
        if (!readable(n.imm)) return false;
        at = n.imm;
        continue;
      }
      if (n.ends) return false;
      at += n.len;
    }
    return false;
  }
  bool flags_dead(const Ins& after) { return dead_from(pc_ + after.len); }

  // Whether, close behind a flag-setting instruction of kind `cls` at pc_,
  // something reads a condition that ARM's flags can answer, with only
  // instructions that leave ARM's flags alone between, and in this block.
  bool chain_ahead(const Ins& after, int cls) {
    if (checked_ || !(g_flags_mode & 2)) return false;
    // COCAINE_JIT_VERIFY calls out after each SSE instruction, which loses
    // ARM's flags before a compare's branch could read them.
    if (g_verify && after.kind == kSse) return false;
    u32 at = pc_ + after.len;
    for (u32 k = 0; k < room_ && k < 4; k++) {
      Ins n;
      if (!decode(mem_, at, n)) return false;
      if (reads_cc(n.kind)) return native_cond(cls, n.op & 15) >= 0;
      if (!transparent(n.kind)) return false;
      at += n.len;
    }
    return false;
  }
  // And whether, once those conditions have been read from ARM's flags, the
  // guest's flags are dead: on the far side of each branch too.
  bool dead_after_chain(const Ins& after, int cls) {
    u32 at = pc_ + after.len;
    std::vector<u32> counted;
    for (u32 k = 0; k < room_ && k < 8; k++) {
      Ins n;
      if (!decode(mem_, at, n)) return false;
      if (reads_cc(n.kind)) {
        if (native_cond(cls, n.op & 15) < 0) return false;
        if (n.kind == kJcc && !(readable(n.imm) && dead_from(n.imm))) return false;
        counted.push_back(at);
      } else if (!transparent(n.kind)) {
        break;
      }
      at += n.len;
    }
    if (!dead_from(at)) return false;
    promised_.insert(promised_.end(), counted.begin(), counted.end());
    return true;
  }
  // A condition is about to be read from eflags. If the instruction that
  // set the flags left no note because this very condition was to read
  // ARM's, the flags are wrong: better to stop than to go on.
  void unfused() {
    for (const u32 at : promised_) {
      if (at != pc_) continue;
      std::fprintf(stderr, "cocaine: internal error: the translator lost ARM's flags before 0x%08x\n", pc_);
      std::abort();
    }
  }

  // What an instruction that sets the flags does about them.
  struct Plan {
    bool native = false;  // something close behind reads a condition from ARM's flags
    bool record = false;  // and the guest's own may be wanted later
  };
  Plan plan(const Ins& n, int cls) {
    Plan f;
    f.native = chain_ahead(n, cls);
    f.record = !(f.native ? dead_after_chain(n, cls) : flags_dead(n));
    return f;
  }
  static int cls_of(u32 which) { return which == 0 ? kAddCls : which == 5 || which == 7 ? kSubCls : kLogicCls; }
  Plan plan_alu(const Ins& n, u32 which) { return which == 2 || which == 3 ? Plan() : plan(n, cls_of(which)); }

  // COCAINE_JIT_POISON: this instruction's flags are being skipped.
  void poison() {
    if (!g_poison) return;
    imm(S1, kPoison);
    e(strw_at(S1, ST, XOFF(lazy_kind)));
    imm(S1, pc_);
    e(strw_at(S1, ST, XOFF(lazy_a)));
    known_clear_ = false;
  }
  // ... and this one reads them.
  void check_poison() {
    if (!g_poison) return;
    e(ldrw_at(S0, ST, XOFF(lazy_kind)));
    e(cmpw_i(S0, kPoison));
    cold_.push_back({kColdPoison, forward(bcond(EQ, 0), 19), pc_, 0});
    cold_.back().back = p;
  }

  // Leaves the note. `b` is a register, or negative for `constant`.
  void record(u32 code, int bits, int a, int b, u32 constant) {
    e(strw_at(a, ST, XOFF(lazy_a)));
    if (b < 0 && constant == 0) {
      b = ZR;
    } else if (b < 0) {
      imm(S1, constant);
      b = S1;
    }
    e(strw_at(b, ST, XOFF(lazy_b)));
    imm(S1, code | (u32)bits << 8);
    e(strw_at(S1, ST, XOFF(lazy_kind)));
    known_clear_ = false;
  }
  // Has eflags worked out, if a note is waiting. This may call out, so it
  // comes before an instruction's translation has anything in a scratch
  // register, and ARM's flags don't survive it.
  void ensure() {
    if (known_clear_) return;
    e(ldrw_at(S0, ST, XOFF(lazy_kind)));
    cold_.push_back({kColdFlush, forward(cbnzw(S0, 0), 19), 0, 0});
    cold_.back().back = p;
    known_clear_ = true;
    native_ = kNoCls;
  }

  // ---- operands ----
  // The address a memory operand names, into T0.
  void ea(const Ins& n, bool segment = true) {
    if (n.has_base && n.index != 4) {
      e(addw(T0, G(n.base), G(n.index), n.scale));
      add_imm(T0, T0, n.disp);
    } else if (n.has_base) {
      add_imm(T0, G(n.base), n.disp);
    } else if (n.index != 4) {
      e(lslw_i(T0, G(n.index), n.scale));
      add_imm(T0, T0, n.disp);
    } else {
      imm(T0, n.disp);
    }
    if (n.gs && segment) {
      e(ldrw_at(S1, ST, XOFF(seg_base) + 4 * (u32)X86State::GS));
      e(addw(T0, T0, S1));
    }
  }
  // A register's value, for reading only. Byte registers are al cl dl bl ah ch dh bh.
  int reg_load(int r, int sz, int scratch) {
    if (sz == 4) return G(r);
    if (r < 4) {
      e(ubfxw(scratch, G(r), 0, 8));
    } else {
      e(ubfxw(scratch, G(r - 4), 8, 8));
    }
    return scratch;
  }
  void reg_store(int r, int sz, int from) {
    if (sz == 4) {
      if (G(r) != from) e(movw(G(r), from));
    } else if (r < 4) {
      e(bfiw(G(r), from, 0, 8));
    } else {
      e(bfiw(G(r - 4), from, 8, 8));
    }
  }
  int rm_load(const Ins& n, int sz, int scratch) {
    if (n.mod == 3) return reg_load(n.rm, sz, scratch);
    ea(n);
    e(sz == 4 ? ldrw_idx(scratch, MEM, T0) : ldrb_idx(scratch, MEM, T0));
    return scratch;
  }
  void rm_store(const Ins& n, int sz, int from) {  // `from` must not be T0
    if (n.mod == 3) {
      reg_store(n.rm, sz, from);
      return;
    }
    ea(n);
    e(sz == 4 ? strw_idx(from, MEM, T0) : strb_idx(from, MEM, T0));
  }

  void push(int value) {  // the store first, so a fault leaves esp alone
    e(subw_i(S0, G(kEsp), 4));
    e(strw_idx(value, MEM, S0));
    e(movw(G(kEsp), S0));
  }

  // ---- the ways out ----
  void site(u32 target) {
    const u32 index = (u32)g.sites.size();
    g.sites.push_back({here(), 0, target});
    cold_.push_back({kColdSite, forward(b(0), 26), target, index});
  }
  void indirect() {  // to the guest address in T0
    e(ldrx_at(S0, ST, XOFF(jit_table)));
    e(ubfxw(S1, T0, 0, kTableBits));
    e(ldrx_scaled(S1, S0, S1));
    e(lsrx_i(S0, S1, 32));
    e(cmpw(S0, T0));
    cold_.push_back({kColdMiss, forward(bcond(NE, 0), 19), 0, 0});
    e(ldrx_at(S0, ST, XOFF(jit_base)));
    e(addx_uxtw(S0, S0, S1));
    e(br(S0));
  }
  void fallback() {  // the instruction at pc_ by the interpreter; the block ends
    imm(S1, pc_);
    call(g.t_step);
    cold_.push_back({kColdStop, forward(cbnzw(S0, 0), 19), 0, 0});
    jump(g.t_lookup_kept);
  }

  // ---- arithmetic ----
  // One of the eight operations on `a` and either `b` (a register) or a
  // constant, the result in T1: not for a compare, and for a test only if
  // `result`. The ones that take the carry in go to the interpreter's
  // routine, which sets the flags as it goes.
  void alu(u32 which, int a, int b, u32 constant, int sz, const Plan& f, bool result = true) {
    if (which == 2 || which == 3) {
      e(movw(2, a));
      if (b >= 0) {
        e(movw(3, b));
      } else {
        imm(3, constant);
      }
      e(movx(0, ST));
      imm(1, which);
      imm(4, (u32)sz * 8);
      call(g.t_alu);
      e(movw(T1, 0));
      known_clear_ = true;
      return;
    }
    if (f.record) {
      record(which + 1, sz * 8, a, b, constant);
    } else {
      poison();
    }
    if (which == 7) result = false;
    if (!f.native) {
      if (!result) return;
      if (b < 0) {
        if (which == 0) {
          add_imm(T1, a, constant);
          return;
        }
        if (which == 5) {
          add_imm(T1, a, 0u - constant);
          return;
        }
        if (which == 1 || which == 4 || which == 6) {
          if (constant == 0) {
            if (which == 4) e(movzw(T1, 0));
            else e(movw(T1, a));
            return;
          }
          if (constant == 0xFFFFFFFFu) {
            if (which == 1) e(mvnw(T1, ZR));
            else if (which == 4) e(movw(T1, a));
            else e(mvnw(T1, a));
            return;
          }
          if (const Mask m = mask32(constant); m.ok) {
            e(which == 1 ? orrw_i(T1, a, m) : which == 4 ? andw_i(T1, a, m)
                                                       : eorw_i(T1, a, m));
            return;
          }
        }
        imm(T2, constant);
        b = T2;
      }
      switch (which) {
        case 0: e(addw(T1, a, b)); break;
        case 1: e(orrw(T1, a, b)); break;
        case 4: e(andw(T1, a, b)); break;
        case 5: e(subw(T1, a, b)); break;
        default: e(eorw(T1, a, b)); break;
      }
      return;
    }
    const bool sum = which == 0, diff = which == 5 || which == 7;
    if (sz == 4) {
      const bool shifted = constant >= 4096 && (constant & 0xFFF) == 0 && (constant >> 12) < 4096;
      if ((sum || diff) && b < 0 && (constant < 4096 || shifted)) {
        const u32 value = shifted ? constant >> 12 : constant;
        if (sum) e(addsw_i(T1, a, value, shifted));
        else e(subsw_i(which == 5 ? T1 : ZR, a, value, shifted));
      } else {
        bool direct = false;
        if (b < 0 && which == 4) {
          if (const Mask m = mask32(constant); m.ok) {
            if (result) {
              e(andw_i(T1, a, m));
              e(tstw(T1, T1));
            } else {
              e(tstw_i(a, m));
            }
            direct = true;
          } else if (constant == 0) {
            e(andsw(result ? T1 : ZR, a, ZR));
            direct = true;
          } else if (constant == 0xFFFFFFFFu) {
            e(andsw(result ? T1 : ZR, a, a));
            direct = true;
          }
        } else if (b < 0 && (which == 1 || which == 6)) {
          if (constant == 0) {
            e(andsw(T1, a, a));
            direct = true;
          } else if (constant == 0xFFFFFFFFu) {
            e(which == 1 ? mvnw(T1, ZR) : mvnw(T1, a));
            e(tstw(T1, T1));
            direct = true;
          } else if (const Mask m = mask32(constant); m.ok) {
            e(which == 1 ? orrw_i(T1, a, m) : eorw_i(T1, a, m));
            e(tstw(T1, T1));
            direct = true;
          }
        }
        if (!direct) {
          if (b < 0) {
            imm(T2, constant);
            b = T2;
          }
          switch (which) {
            case 0: e(addsw(T1, a, b)); break;
            case 5: e(subsw(T1, a, b)); break;
            case 7: e(cmpw(a, b)); break;
            case 4: e(andsw(T1, a, b)); break;
            default:
              e(which == 1 ? orrw(T1, a, b) : eorw(T1, a, b));
              e(tstw(T1, T1));
              break;
          }
        }
      }
    } else if (sum || diff || which == 4) {
      // A byte: done at the top of the register, where ARM's flags come out
      // as an 8-bit operation's would.
      u32 up = 24;
      e(lslw_i(S0, a, 24));
      if (b < 0) {
        imm(T2, constant << 24);
        b = T2;
        up = 0;
      }
      e(sum ? addsw(S0, S0, b, up) : diff ? subsw(S0, S0, b, up) : andsw(S0, S0, b, up));
      if (result) e(lsrw_i(T1, S0, 24));
    } else {
      if (b < 0) {
        imm(T2, constant);
        b = T2;
      }
      e(which == 1 ? orrw(T1, a, b) : eorw(T1, a, b));
      e(lslw_i(S0, T1, 24));
      e(tstw(S0, S0));
    }
    native_ = cls_of(which);
    keep_ = true;
  }
  // inc and dec, the result in T1. They leave the carry flag as it was, so
  // their note needs the carry worked out before it is left: the caller has
  // called ensure() for that, before loading anything.
  void inc_dec(int a, bool dec, const Plan& f) {
    if (f.record) {
      e(strw_at(a, ST, XOFF(lazy_a)));
      imm(S1, (kLazyInc + (dec ? 1u : 0u)) | 32u << 8);
      e(strw_at(S1, ST, XOFF(lazy_kind)));
      known_clear_ = false;
    } else {
      poison();
    }
    if (f.native) {
      e(dec ? subsw_i(T1, a, 1) : addsw_i(T1, a, 1));
      native_ = kStepCls;
      keep_ = true;
    } else {
      e(dec ? subw_i(T1, a, 1) : addw_i(T1, a, 1));
    }
  }
  // After a multiply whose full product is in x0: the carry and overflow
  // flags say whether it fits in 32 bits, and the rest stay as they were.
  void imul_flags() {
    e(cmpx_sxtw(0, 0));
    e(csetw(S1, NE));
    e(ldrw_at(S0, ST, XOFF(eflags)));
    imm(T2, 0x801);
    e(bicw(S0, S0, T2));
    e(orrw(S0, S0, S1));
    e(orrw(S0, S0, S1, 11));
    e(strw_at(S0, ST, XOFF(eflags)));
  }

  // A branch taken when condition `cc` does not hold. eflags must be in S0.
  Fix unless(u32 cc) {
    const bool want = !(cc & 1);  // the condition holds when the tested bit equals this
    u32 bit;
    int reg = S0;
    switch (cc >> 1) {
      case 0: bit = 11; break;  // overflow
      case 1: bit = 0; break;   // carry
      case 2: bit = 6; break;   // zero
      case 3:                   // carry or zero
        e(orrw_lsr(S1, S0, S0, 6));
        reg = S1;
        bit = 0;
        break;
      case 4: bit = 7; break;  // sign
      case 5: bit = 2; break;  // parity
      case 6:                  // sign differs from overflow
        e(eorw_lsr(S1, S0, S0, 4));
        reg = S1;
        bit = 7;
        break;
      default:  // that, or zero
        e(eorw_lsr(S1, S0, S0, 4));
        e(orrw(S1, S1, S0, 1));
        reg = S1;
        bit = 7;
        break;
    }
    return forward(want ? tbzw(reg, bit, 0) : tbnzw(reg, bit, 0), 14);
  }

  // One instruction. False if it has to go to the interpreter after all.
  bool one(const Ins& n);
  bool sse(const Ins& n);
  bool x87(const Ins& n);
};

bool Translator::one(const Ins& n) {
  const u32 next = pc_ + n.len;
  const int sz = n.sz;
  const u32 mask = sz == 4 ? 0xFFFFFFFFu : 0xFFu;
  switch (n.kind) {
    case kAluRmR: {
      const Plan f = plan_alu(n, n.imm);
      const int a = rm_load(n, sz, T1), b = reg_load(n.reg, sz, T2);
      alu(n.imm, a, b, 0, sz, f);
      if (n.imm != 7) rm_store(n, sz, T1);
      return true;
    }
    case kAluRRm: {
      const Plan f = plan_alu(n, n.imm);
      const int b = rm_load(n, sz, T2), a = reg_load(n.reg, sz, T1);
      alu(n.imm, a, b, 0, sz, f);
      if (n.imm != 7) reg_store(n.reg, sz, T1);
      return true;
    }
    case kAluAImm: {
      const Plan f = plan_alu(n, n.imm);
      alu(n.imm, reg_load(0, sz, T1), -1, n.disp & mask, sz, f);
      if (n.imm != 7) reg_store(0, sz, T1);
      return true;
    }
    case kGroup1: {
      const Plan f = plan_alu(n, n.reg);
      alu(n.reg, rm_load(n, sz, T1), -1, n.imm & mask, sz, f);
      if (n.reg != 7) rm_store(n, sz, T1);
      return true;
    }
    case kTestRmR: {
      const Plan f = plan(n, kLogicCls);
      if (!f.native && !f.record) {
        poison();
        return true;
      }
      const int a = rm_load(n, sz, T1), b = reg_load(n.reg, sz, T2);
      alu(4, a, b, 0, sz, f, false);
      return true;
    }
    case kTestAImm: {
      const Plan f = plan(n, kLogicCls);
      if (f.native || f.record) {
        alu(4, reg_load(0, sz, T1), -1, n.imm & mask, sz, f, false);
      } else {
        poison();
      }
      return true;
    }
    case kIncDecReg: {
      const int r = n.op & 7;
      const Plan f = plan(n, kStepCls);
      if (f.record) ensure();
      inc_dec(G(r), n.op >= 0x48, f);
      e(movw(G(r), T1));
      return true;
    }
    case kPushReg:
      push(G(n.op & 7));
      return true;
    case kPopReg:
      e(ldrw_idx(G(n.op & 7), MEM, G(kEsp)));
      e(addw_i(G(kEsp), G(kEsp), 4));
      return true;
    case kPushImm:
      imm(T1, n.imm);
      push(T1);
      return true;
    case kJcc: {
      const int c = native_cond(native_, n.op & 15);
      if (c >= 0) {  // straight from ARM's flags
        const Fix skip = forward(bcond(invert((Cond)c), 0), 19);
        site(n.imm);
        bind(skip);
        keep_ = true;
        return true;
      }
      unfused();
      check_poison();
      ensure();
      e(ldrw_at(S0, ST, XOFF(eflags)));
      const Fix skip = unless(n.op & 15);
      site(n.imm);
      bind(skip);
      return true;
    }
    case kMovRmR:
      rm_store(n, sz, reg_load(n.reg, sz, T1));
      return true;
    case kMovRRm:
      reg_store(n.reg, sz, rm_load(n, sz, T1));
      return true;
    case kLea:
      ea(n, false);
      e(movw(G(n.reg), T0));
      return true;
    case kMovRImm:
      if (sz == 4) {
        imm(G(n.op & 7), n.imm);
      } else {
        imm(T1, n.imm);
        reg_store(n.op & 7, 1, T1);
      }
      return true;
    case kMovRmImm:
      imm(T1, n.imm);
      rm_store(n, sz, T1);
      return true;
    case kMovAMem:
    case kMovMemA: {
      Ins at = n;
      at.mod = 0;
      at.has_base = false;
      at.index = 4;
      ea(at);
      e(n.kind == kMovAMem ? ldrw_idx(G(0), MEM, T0) : strw_idx(G(0), MEM, T0));
      return true;
    }
    case kShift: {
      const bool by_cl = n.op == 0xD3;
      if (!by_cl && n.imm == 0) return true;
      if (!by_cl && (n.reg == 4 || n.reg == 5 || n.reg == 7) && flags_dead(n)) {
        const int a = rm_load(n, 4, T1);
        e(n.reg == 4 ? lslw_i(T1, a, n.imm) : n.reg == 5 ? lsrw_i(T1, a, n.imm) : asrw_i(T1, a, n.imm));
        poison();
        rm_store(n, 4, T1);
        return true;
      }
      e(movw(2, rm_load(n, 4, T1)));
      if (by_cl) {
        e(movw(3, G(1)));
      } else {
        imm(3, n.imm);
      }
      e(movx(0, ST));
      imm(1, n.reg);
      imm(4, 32);
      call(g.t_shift);
      e(movw(T1, 0));
      known_clear_ = true;
      rm_store(n, 4, T1);
      return true;
    }
    case kRet:
      e(ldrw_idx(T0, MEM, G(kEsp)));
      add_imm(G(kEsp), G(kEsp), 4 + n.imm);
      indirect();
      return true;
    case kCall:
      imm(T1, next);
      push(T1);
      site(n.imm);
      return true;
    case kJmp:
      site(n.imm);
      return true;
    case kGroup5:
      switch (n.reg) {
        case 0:
        case 1: {
          const Plan f = plan(n, kStepCls);
          if (f.record) ensure();
          inc_dec(rm_load(n, 4, T1), n.reg == 1, f);
          rm_store(n, 4, T1);
          return true;
        }
        case 2:  // call through a register or memory: the target is read first
          e(movw(T2, rm_load(n, 4, T1)));
          imm(T1, next);
          push(T1);
          e(movw(T0, T2));
          indirect();
          return true;
        case 4:
          e(movw(T2, rm_load(n, 4, T1)));
          e(movw(T0, T2));
          indirect();
          return true;
        default:
          push(rm_load(n, 4, T1));
          return true;
      }
    case kGroup3:
      if (n.reg == 0) {
        const Plan f = plan(n, kLogicCls);
        if (f.native || f.record) {
          alu(4, rm_load(n, sz, T1), -1, n.imm & mask, sz, f, false);
        } else {
          poison();
        }
        return true;
      }
      if (n.reg == 3) {  // neg: nought minus it, flags and all
        const Plan f = plan(n, kSubCls);
        const int a = rm_load(n, 4, T1);
        if (f.record) {
          record(6, 32, ZR, a, 0);
        } else {
          poison();
        }
        e(f.native ? subsw(T1, ZR, a) : negw(T1, a));
        if (f.native) {
          native_ = kSubCls;
          keep_ = true;
        }
        rm_store(n, 4, T1);
        return true;
      }
      {
        const int a = rm_load(n, 4, T1);
        e(mvnw(T1, a));
        rm_store(n, 4, T1);
      }
      return true;
    case kLeave:
      e(movw(G(kEsp), G(kEbp)));
      e(ldrw_idx(G(kEbp), MEM, G(kEsp)));
      e(addw_i(G(kEsp), G(kEsp), 4));
      return true;
    case kNop:
      return true;
    case kCdq:
      e(asrw_i(G(2), G(0), 31));
      return true;
    case kMovX: {
      const bool word = n.op & 1, sign = n.op >= 0xBE;
      if (!word) {
        const int v = rm_load(n, 1, T1);
        e(sign ? sxtbw(T1, v) : movw(T1, v));
      } else {
        if (n.mod == 3) {
          e(ubfxw(T1, G(n.rm), 0, 16));
        } else {
          ea(n);
          e(ldrh_idx(T1, MEM, T0));
        }
        if (sign) e(sxthw(T1, T1));
      }
      e(movw(G(n.reg), T1));
      return true;
    }
    case kImul2:
    case kImul3: {
      const bool dead = flags_dead(n);
      if (!dead) ensure();
      const int v = rm_load(n, 4, T1);
      if (n.kind == kImul3) {
        const u32 factor = (u32)n.imm;
        const int d = G(n.reg);
        if (dead && factor == 0) {
          e(movzw(d, 0));
        } else if (dead && factor == 1) {
          if (d != v) e(movw(d, v));
        } else if (dead && factor == 0xFFFFFFFFu) {
          e(negw(d, v));
        } else if (dead && (factor & (factor - 1)) == 0) {
          e(lslw_i(d, v, __builtin_ctz(factor)));
        } else {
          imm(T2, factor);
          e(dead ? mulw(d, v, T2) : smull(0, v, T2));
        }
      } else if (dead) {
        e(mulw(G(n.reg), v, G(n.reg)));
      } else {
        e(smull(0, v, G(n.reg)));
      }
      if (dead) {
        poison();
      } else {
        imul_flags();
        e(movw(G(n.reg), 0));
      }
      return true;
    }
    case kSse: {
      if (!g_verify) return sse(n);
      imm(S1, pc_);
      call(g.t_verify);
      native_ = kNoCls;
      known_clear_ = false;
      const bool ok = sse(n);
      imm(S1, 0x80000000u | pc_);
      call(g.t_verify);
      return ok;
    }
    case kX87:
      return x87(n);
    case kBitScan: {  // the number of the lowest or highest set bit; with none, the register is left alone
      const bool dead = flags_dead(n);
      if (!dead) ensure();
      const int v = rm_load(n, 4, T1);
      if (n.op == 0xBC) {
        e(rbitw(T2, v));
        e(clzw(T2, T2));
      } else {
        e(clzw(T2, v));
        e(eorw_i(T2, T2, mask32(31)));
      }
      e(cmpw_i(v, 0));
      if (!dead) {
        e(ldrw_at(S0, ST, XOFF(eflags)));
        e(csetw(S1, EQ));
        e(bfiw(S0, S1, 6, 1));
        e(strw_at(S0, ST, XOFF(eflags)));
      } else {
        poison();
      }
      e(cselw(G(n.reg), G(n.reg), T2, EQ));
      return true;
    }
    case kSetcc: {
      const int c = native_cond(native_, n.op & 15);
      if (c >= 0) {
        e(csetw(T1, (Cond)c));
        rm_store(n, 1, T1);
        keep_ = true;
        return true;
      }
      unfused();
      check_poison();
      ensure();
      e(ldrw_at(S0, ST, XOFF(eflags)));
      e(movzw(T1, 0));
      const Fix skip = unless(n.op & 15);
      e(movzw(T1, 1));
      bind(skip);
      rm_store(n, 1, T1);
      return true;
    }
    case kCmov: {
      const int c = native_cond(native_, n.op & 15);
      if (c >= 0) {
        const int v = rm_load(n, 4, T1);
        e(cselw(G(n.reg), v, G(n.reg), (Cond)c));
        keep_ = true;
        return true;
      }
      unfused();
      check_poison();
      ensure();
      const int v = rm_load(n, 4, T1);  // read whether or not it is moved
      e(ldrw_at(S0, ST, XOFF(eflags)));
      const Fix skip = unless(n.op & 15);
      e(movw(G(n.reg), v));
      bind(skip);
      return true;
    }
  }
  return false;
}

// An x87 instruction. The interpreter's routine does the work, on the 80-bit
// registers in the X86State, but it is called from here and the block
// carries on: no trip through the interpreter's front door.
bool Translator::x87(const Ins& n) {
  const u32 modrm = (u32)(n.mod << 6 | n.reg << 3 | n.rm);
  if (n.op == 0xDF && modrm == 0xE0) {  // fnstsw ax, whose ax is in a host register here
    imm(S1, XOFF(fsw));
    e(ldrh_idx(S0, ST, S1));
    e(bfiw(G(0), S0, 0, 16));  // eax
    return true;
  }
  if (n.mod == 3) {
    e(movzw(3, 0));
  } else {
    // It reads or writes memory itself, so a bad address is found out in
    // there: the registers and the pc go where the report looks for them.
    ea(n);
    e(movw(3, T0));
    spill();
    imm(S1, pc_);
    e(strw_at(S1, ST, XOFF(eip)));
  }
  e(movx(0, ST));
  imm(1, n.op);
  imm(2, modrm);
  call(g.t_x87);
  known_clear_ = true;
  cold_.push_back({kColdStep, forward(cbzw(0, 0), 19), pc_, 0});
  cold_.back().back = p;
  return true;
}

// SSE. The registers stay in the X86State: a scalar instruction reads and
// writes only the low part there, which leaves the rest of the register as
// it was, as the instruction is meant to. Arithmetic is the host's own, in
// the guest's rounding mode, exactly as the interpreter does it.
bool Translator::sse(const Ins& n) {
  const u32 prefix = n.imm;
  const u32 dst = XOFF(xmm) + 16 * (u32)n.reg, src = XOFF(xmm) + 16 * (u32)n.rm;
  const bool reg = n.mod == 3, wide = prefix == 0xF2 || prefix == 0x66;  // doubles, not singles
  auto low = [&](bool is64) {  // the second operand's low part into d1 or s1
    if (reg) {
      e(is64 ? ldrd_at(1, ST, src) : ldrs_at(1, ST, src));
    } else {
      ea(n);
      e(is64 ? ldrd_idx(1, MEM, T0) : ldrs_idx(1, MEM, T0));
    }
  };
  auto whole_in = [&](int q, u32 at) {
    e(addx_i(S0, ST, at));
    e(ldrq_at(q, S0, 0));
  };
  auto whole_out = [&](int q, u32 at) {
    e(addx_i(S0, ST, at));
    e(strq_at(q, S0, 0));
  };
  auto whole_src = [&](int q) {  // all of the second operand
    if (reg) {
      whole_in(q, src);
    } else {
      ea(n);
      e(ldrq_idx(q, MEM, T0));
    }
  };
  switch (n.op) {
    case 0x10:
    case 0x28:
    case 0x6F:  // into a register
      if (n.op == 0x10 && (prefix == 0xF2 || prefix == 0xF3)) {  // movsd, movss
        low(prefix == 0xF2);
        if (prefix == 0xF2) {
          e(strd_at(1, ST, dst));
          if (!reg) e(strx_at(ZR, ST, dst + 8));  // from memory, the rest is cleared
        } else {
          e(strs_at(1, ST, dst));
          if (!reg) {
            e(strw_at(ZR, ST, dst + 4));
            e(strx_at(ZR, ST, dst + 8));
          }
        }
        return true;
      }
      if (reg) {
        whole_in(1, src);
      } else {
        ea(n);
        e(ldrq_idx(1, MEM, T0));
      }
      whole_out(1, dst);
      return true;
    case 0x11:
    case 0x29:
    case 0x7F:  // out of a register
      if (n.op == 0x11 && (prefix == 0xF2 || prefix == 0xF3)) {
        const bool is64 = prefix == 0xF2;
        e(is64 ? ldrd_at(0, ST, dst) : ldrs_at(0, ST, dst));
        if (reg) {
          e(is64 ? strd_at(0, ST, src) : strs_at(0, ST, src));
        } else {
          ea(n);
          e(is64 ? strd_idx(0, MEM, T0) : strs_idx(0, MEM, T0));
        }
        return true;
      }
      whole_in(0, dst);
      if (reg) {
        whole_out(0, src);
      } else {
        ea(n);
        e(strq_idx(0, MEM, T0));
      }
      return true;
    case 0x12:  // the low half from memory, or from the other's high half
      if (reg) {
        e(ldrd_at(1, ST, src + 8));
      } else {
        ea(n);
        e(ldrd_idx(1, MEM, T0));
      }
      e(strd_at(1, ST, dst));
      return true;
    case 0x16:  // the high half from memory, or from the other's low half
      low(true);
      e(strd_at(1, ST, dst + 8));
      return true;
    case 0x17:  // and to memory
      e(ldrd_at(0, ST, dst + 8));
      ea(n);
      e(strd_idx(0, MEM, T0));
      return true;
    case 0x14:
    case 0x15:  // the low or high halves of both, interleaved
      whole_src(1);
      whole_in(0, dst);
      e(n.op == 0x14 ? zip1v(wide ? 3 : 2, 0, 0, 1) : zip2v(wide ? 3 : 2, 0, 0, 1));
      whole_out(0, dst);
      return true;
    case 0x60: case 0x61: case 0x62: case 0x6C:
    case 0x68: case 0x69: case 0x6A: case 0x6D: {
      const u32 size = (n.op & 7) > 2 ? 3 : (n.op & 7);
      whole_src(1);
      whole_in(0, dst);
      e(n.op < 0x68 || n.op == 0x6C ? zip1v(size, 0, 0, 1) : zip2v(size, 0, 0, 1));  // 60 61 62 6C are the low ones
      whole_out(0, dst);
      return true;
    }
    case 0x70: {  // four words picked from the second operand
      if (reg) {
        e(addx_i(S0, ST, src));
      } else {
        ea(n);
        e(addx_uxtw(S0, MEM, T0));
      }
      for (u32 k = 0; k < 4; k++) e(ldrw_at((int)k, S0, 4 * ((n.ib >> (2 * k)) & 3)));
      for (u32 k = 0; k < 4; k++) e(strw_at((int)k, ST, dst + 4 * k));
      return true;
    }
    case 0xC6: {  // the low half picked from the first operand, the high half from the second
      if (reg) {
        e(addx_i(S0, ST, src));
      } else {
        ea(n);
        e(addx_uxtw(S0, MEM, T0));
      }
      if (wide) {
        e(ldrx_at(0, ST, dst + 8 * (n.ib & 1)));
        e(ldrx_at(1, S0, 8 * ((n.ib >> 1) & 1)));
        e(strx_at(0, ST, dst));
        e(strx_at(1, ST, dst + 8));
        return true;
      }
      e(ldrw_at(0, ST, dst + 4 * (n.ib & 3)));
      e(ldrw_at(1, ST, dst + 4 * ((n.ib >> 2) & 3)));
      e(ldrw_at(2, S0, 4 * ((n.ib >> 4) & 3)));
      e(ldrw_at(3, S0, 4 * ((n.ib >> 6) & 3)));
      for (u32 k = 0; k < 4; k++) e(strw_at((int)k, ST, dst + 4 * k));
      return true;
    }
    case 0x71:
    case 0x72:
    case 0x73: {  // each element shifted by a constant; x86 lets the constant be any size
      const u32 size = n.op - 0x70u, width = 8u << size;
      whole_in(0, src);
      if (n.reg == 4) {
        if (n.ib) e(sshrv(size, 0, 0, n.ib < width ? n.ib : width - 1));
      } else if (n.ib >= width) {
        e(eorv(0, 0, 0));
      } else if (n.ib) {
        e(n.reg == 2 ? ushrv(size, 0, 0, n.ib) : shlv(size, 0, 0, n.ib));
      }
      whole_out(0, src);
      return true;
    }
    case 0xC2: {  // a compare that leaves all ones or all zeros
      const bool scalar = prefix == 0xF2 || prefix == 0xF3, d = prefix == 0xF2 || prefix == 0x66;
      if (scalar) {
        low(d);
        e(d ? ldrd_at(0, ST, dst) : ldrs_at(0, ST, dst));
      } else {
        whole_src(1);
        whole_in(0, dst);
      }
      switch (n.ib & 3) {
        case 0: e(fcmeqv(d, 2, 0, 1)); break;  // equal
        case 1: e(fcmgtv(d, 2, 1, 0)); break;  // less
        case 2: e(fcmgev(d, 2, 1, 0)); break;  // less or equal
        default:                               // both are numbers
          e(fcmeqv(d, 2, 0, 0));
          e(fcmeqv(d, 3, 1, 1));
          e(andv(2, 2, 3));
          break;
      }
      // The first four are the opposites of the last four, except that
      // "both are numbers" is the last of its pair.
      if (((n.ib & 4) != 0) != ((n.ib & 3) == 3)) e(mvnv(2, 2));
      if (scalar) {
        e(d ? strd_at(2, ST, dst) : strs_at(2, ST, dst));
      } else {
        whole_out(2, dst);
      }
      return true;
    }
    case 0x5B: {  // four 32-bit integers to singles, or back
      whole_src(1);
      if (prefix == 0) {
        e(scvtfv(false, 0, 1));
        whole_out(0, dst);
        return true;
      }
      if (prefix == 0x66) e(frintiv(false, 1, 1));  // by the current rounding; F3 is towards zero
      e(fcvtzsv(false, 0, 1));
      // What doesn't fit, and what isn't a number, has x86's own answer.
      // Too large shows as the largest integer, which no single reaches.
      e(fcmeqv(false, 2, 1, 1));
      e(mvni_max32(3));
      e(cmeqv(2, 3, 0, 3));
      e(bicv(2, 2, 3));
      e(fmov_xd(0, 2));
      e(umov_xd1(1, 2));
      e(andx(0, 0, 1));
      e(cmnx_i(0, 1));
      cold_.push_back({kColdStep, forward(bcond(NE, 0), 19), pc_, 0});
      whole_out(0, dst);
      cold_.back().back = p;
      return true;
    }
    case 0xE6:  // two 32-bit integers to doubles
      low(true);
      e(sxtl_2d(0, 1));
      e(scvtfv(true, 0, 0));
      whole_out(0, dst);
      return true;
    case 0x13:  // and to it
      e(ldrd_at(0, ST, dst));
      ea(n);
      e(strd_idx(0, MEM, T0));
      return true;
    case 0x2A: {  // a 32-bit integer to floating point
      const int v = rm_load(n, 4, T1);
      if (prefix == 0xF2) {
        e(scvtf_dw(0, v));
        e(strd_at(0, ST, dst));
      } else {
        e(scvtf_sw(0, v));
        e(strs_at(0, ST, dst));
      }
      return true;
    }
    case 0x2C:
    case 0x2D: {  // to a 32-bit integer: towards zero, or by the current rounding
      const bool is64 = prefix == 0xF2;
      const size_t first = cold_.size();
      low(is64);
      // x86 has an answer of its own for what doesn't fit and for what isn't
      // a number: the interpreter gives it.
      e(is64 ? fcmpd(1, 1) : fcmps(1, 1));
      cold_.push_back({kColdStep, forward(bcond(VS, 0), 19), pc_, 0});
      if (n.op == 0x2D) e(is64 ? frintid(1, 1) : frintis(1, 1));
      e(is64 ? fcvtzs_xd(0, 1) : fcvtzs_xs(0, 1));
      e(cmpx_sxtw(0, 0));
      cold_.push_back({kColdStep, forward(bcond(NE, 0), 19), pc_, 0});
      e(movw(G(n.reg), 0));
      cold_[first].back = cold_[first + 1].back = p;
      return true;
    }
    case 0x5A: {  // between single and double
      if (prefix == 0 || prefix == 0x66) {  // two at once; the singles sit in the low half
        if (prefix == 0) {
          low(true);
          e(fcvtl_2d(0, 1));
        } else {
          whole_src(1);
          e(fcvtn_2s(0, 1));
        }
        e(fcmeqv(prefix == 0, 2, 0, 0));
        e(fmov_xd(0, 2));
        e(umov_xd1(1, 2));
        e(andx(0, 0, 1));
        e(cmnx_i(0, 1));
        cold_.push_back({kColdStep, forward(bcond(NE, 0), 19), pc_, 0});
        whole_out(0, dst);
        cold_.back().back = p;
        return true;
      }
      const bool to_single = prefix == 0xF2;
      low(to_single);
      e(to_single ? fcvt_sd(0, 1) : fcvt_ds(0, 1));
      e(to_single ? fcmps(0, 0) : fcmpd(0, 0));
      cold_.push_back({kColdStep, forward(bcond(VS, 0), 19), pc_, 0});
      e(to_single ? strs_at(0, ST, dst) : strd_at(0, ST, dst));
      cold_.back().back = p;
      return true;
    }
    case 0x5D:
    case 0x5F: {  // the smaller or the larger: the first if it wins outright, otherwise the second
      const bool max = n.op == 0x5F;
      if (prefix == 0xF2 || prefix == 0xF3) {
        const bool is64 = prefix == 0xF2;
        low(is64);
        e(is64 ? ldrd_at(0, ST, dst) : ldrs_at(0, ST, dst));
        e(is64 ? fcmpd(0, 1) : fcmps(0, 1));
        e(is64 ? fcselD(0, 0, 1, max ? GT : MI) : fcselS(0, 0, 1, max ? GT : MI));
        e(is64 ? strd_at(0, ST, dst) : strs_at(0, ST, dst));
        return true;
      }
      whole_src(1);
      whole_in(0, dst);
      e(max ? fcmgtv(wide, 2, 0, 1) : fcmgtv(wide, 2, 1, 0));
      e(bslv(2, 0, 1));
      whole_out(2, dst);
      return true;
    }
    case 0x6E: {  // a 32-bit integer in, the rest cleared
      const int v = rm_load(n, 4, T1);
      e(strw_at(v, ST, dst));
      e(strw_at(ZR, ST, dst + 4));
      e(strx_at(ZR, ST, dst + 8));
      return true;
    }
    case 0x7E:
      if (prefix == 0x66) {  // the low 32 bits out
        e(ldrw_at(T1, ST, dst));
        rm_store(n, 4, T1);
        return true;
      }
      low(true);  // the low 64 bits in, the rest cleared
      e(strd_at(1, ST, dst));
      e(strx_at(ZR, ST, dst + 8));
      return true;
    case 0xD6:  // the low 64 bits out
      e(ldrd_at(0, ST, dst));
      if (reg) {
        e(strd_at(0, ST, src));
        e(strx_at(ZR, ST, src + 8));
      } else {
        ea(n);
        e(strd_idx(0, MEM, T0));
      }
      return true;
    case 0xD7:  // the top bit of each of the sixteen bytes, as one number
      e(ldrx_at(0, ST, src));
      e(ldrx_at(1, ST, src + 8));
      imm64(S1, 0x0002040810204081ull);  // times this, eight spaced bits land together in the top byte
      e(andx_tops(0, 0));
      e(andx_tops(1, 1));
      e(mulx(0, 0, S1));
      e(mulx(1, 1, S1));
      e(lsrx_i(0, 0, 56));
      e(lsrx_i(1, 1, 56));
      e(orrw(G(n.reg), 0, 1, 8));
      return true;
    case 0x2E:
    case 0x2F: {  // compare, into the flags: unordered sets three, less one, equal one
      const Plan f = plan(n, kFloatCls);
      if (!f.native && !f.record) {
        poison();
        return true;
      }
      low(wide);
      e(wide ? ldrd_at(0, ST, dst) : ldrs_at(0, ST, dst));
      e(wide ? fcmpd(0, 1) : fcmps(0, 1));
      if (f.native) {
        native_ = kFloatCls;
        keep_ = true;
      }
      if (!f.record) return true;
      e(csetw(T1, LT));  // carry: less, or unordered
      e(csetw(T2, VS));  // parity: unordered
      e(csetw(S1, EQ));
      e(orrw(S1, S1, T2));  // zero: equal, or unordered
      e(ldrw_at(S0, ST, XOFF(eflags)));
      imm(T0, 0x8D5);
      e(bicw(S0, S0, T0));
      e(orrw(S0, S0, T1));
      e(orrw(S0, S0, T2, 2));
      e(orrw(S0, S0, S1, 6));
      e(strw_at(S0, ST, XOFF(eflags)));
      e(strw_at(ZR, ST, XOFF(lazy_kind)));  // all six set here: any note is out of date
      known_clear_ = true;
      return true;
    }
    case 0x51:
    case 0x58:
    case 0x59:
    case 0x5C:
    case 0x5E: {
      if (prefix == 0 || prefix == 0x66) {  // on all four singles, or both doubles
        whole_src(1);
        whole_in(0, dst);
        switch (n.op) {
          case 0x51: e(fsqrtv(wide, 0, 1)); break;
          case 0x58: e(faddv(wide, 0, 0, 1)); break;
          case 0x59: e(fmulv(wide, 0, 0, 1)); break;
          case 0x5C: e(fsubv(wide, 0, 0, 1)); break;
          default: e(fdivv(wide, 0, 0, 1)); break;
        }
        // Not a number in any of them: the interpreter's turn, as below.
        e(fcmeqv(wide, 2, 0, 0));
        e(fmov_xd(0, 2));
        e(umov_xd1(1, 2));
        e(andx(0, 0, 1));
        e(cmnx_i(0, 1));
        cold_.push_back({kColdStep, forward(bcond(NE, 0), 19), pc_, 0});
        whole_out(0, dst);
        cold_.back().back = p;
        return true;
      }
      // On the low part only.
      const bool is64 = prefix == 0xF2;
      low(is64);
      e(is64 ? ldrd_at(0, ST, dst) : ldrs_at(0, ST, dst));
      switch (n.op) {
        case 0x51: e(is64 ? fsqrtd(0, 1) : fsqrts(0, 1)); break;
        case 0x58: e(is64 ? faddd(0, 0, 1) : fadds(0, 0, 1)); break;
        case 0x59: e(is64 ? fmuld(0, 0, 1) : fmuls(0, 0, 1)); break;
        case 0x5C: e(is64 ? fsubd(0, 0, 1) : fsubs(0, 0, 1)); break;
        default: e(is64 ? fdivd(0, 0, 1) : fdivs(0, 0, 1)); break;
      }
      // A result that is not a number: x86 and this Mac disagree about which
      // one it should be, so the interpreter, which knows x86's rule, does
      // the instruction instead. Nothing has been stored yet.
      e(is64 ? fcmpd(0, 0) : fcmps(0, 0));
      cold_.push_back({kColdStep, forward(bcond(VS, 0), 19), pc_, 0});
      e(is64 ? strd_at(0, ST, dst) : strs_at(0, ST, dst));
      cold_.back().back = p;
      return true;
    }
    default: {  // on all 128 bits: the bitwise ones, and integers added, subtracted and compared
      whole_src(1);
      whole_in(0, dst);
      switch (n.op) {
        case 0x54: case 0xDB: e(andv(0, 0, 1)); break;
        case 0x55: case 0xDF: e(bicv(0, 1, 0)); break;  // the second, where the first has no bit
        case 0x56: case 0xEB: e(orrv(0, 0, 1)); break;
        case 0x57: case 0xEF: e(eorv(0, 0, 1)); break;
        case 0xFC: case 0xFD: case 0xFE: e(addv(n.op - 0xFCu, 0, 0, 1)); break;
        case 0xD4: e(addv(3, 0, 0, 1)); break;
        case 0xD5: e(mulv(1, 0, 0, 1)); break;
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: e(subv(n.op - 0xF8u, 0, 0, 1)); break;
        case 0x74: case 0x75: case 0x76: e(cmeqv(n.op - 0x74u, 0, 0, 1)); break;
        case 0x64: case 0x65: case 0x66: e(cmgtv(n.op - 0x64u, 0, 0, 1)); break;
        default: return false;
      }
      whole_out(0, dst);
      return true;
    }
  }
}

// ---- keeping blocks ----------------------------------------------------------

void point_site(u32 index, u32 to) {
  u32* const at = (u32*)(g.base + g.sites[index].patch);
  *at = b((s64)to - (s64)g.sites[index].patch);
  sys_icache_invalidate(at, 4);
}

Block* block_for(const u8* mem, u32 pc) {  // with the lock held
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
  const u32 last = pc + block.bytes - 1;
  for (u32 page = pc >> 12; page <= last >> 12; page++) g.by_page[page].push_back(index);
  return &block;
}

inline u64& table_entry(u32 pc) { return g.table[pc & ((1u << kTableBits) - 1)]; }

u8* code_for(const X86State& s) {
  const u32 pc = s.eip;
  if (g_writable_interpreted && x86_jit_writable && x86_jit_writable(pc)) return nullptr;
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

void link(const X86State& s, u32 site) {
  std::lock_guard<std::mutex> lk(g.mu);
  if (site >= g.sites.size()) return;
  if (g_writable_interpreted && x86_jit_writable && x86_jit_writable(g.sites[site].target)) return;
  Block* block = block_for(s.mem, g.sites[site].target);
  if (!block) return;
  Writing w;
  point_site(site, block->code);
  block->incoming.push_back(site);
}

bool enabled() {
  static const bool on = [] {
    g_counting = getenv("COCAINE_JIT_STATS") != nullptr;
    if (const char* m = getenv("COCAINE_JIT_MATHS")) g_maths = atoi(m) != 0;
    g_verify = getenv("COCAINE_JIT_VERIFY") != nullptr;
    g_barriers = getenv("COCAINE_JIT_BARRIERS") != nullptr;
    g_poison = getenv("COCAINE_JIT_POISON") != nullptr;
    if (const char* w = getenv("COCAINE_JIT_WRITABLE")) g_writable_interpreted = atoi(w) == 0;
    if (const char* f = getenv("COCAINE_JIT_FLAGS")) g_flags_mode = atoi(f) & 15;
    if (const char* k = getenv("COCAINE_JIT_SKIP")) {
      for (const char* q = k; *q;) {
        char* end = nullptr;
        const unsigned long v = strtoul(q, &end, 16);
        if (end == q) break;
        if (v < 256) g_skip[v] = true;
        q = *end ? end + 1 : end;
      }
    }
    const char* e = getenv("COCAINE_JIT");
    return !e || atoi(e) != 0;
  }();
  return on;
}

}  // namespace

void x86_jit_invalidate(u32 addr, u64 size) {
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

bool x86_jit_fault(const void* context, X86State& s) {
  const auto* uc = (const ucontext_t*)context;
  if (!uc || !uc->uc_mcontext || !g.base) return false;
  const auto& ss = uc->uc_mcontext->__ss;
  const u8* at = (const u8*)__darwin_arm_thread_state64_get_pc(ss);
  if (at < g.base || at >= g.base + kRegionSize) return false;
  if (!g.mu.try_lock()) return false;  // a fault handler never waits
  bool found = false;
  const u32 offset = (u32)(at - g.base);
  auto it = g.by_code.upper_bound(offset);
  if (it != g.by_code.begin()) {
    const Block& block = g.blocks[(--it)->second];
    if (offset < block.code_end && !block.at.empty()) {
      size_t n = 0;
      while (n + 1 < block.at.size() && block.at[n + 1].first <= offset) n++;
      s.eip = block.at[n].second;
      for (int r = 0; r < 8; r++) s.reg[r] = (u32)ss.__x[G(r)];
      flush_flags(&s);
      found = true;
    }
  }
  g.mu.unlock();
  return found;
}

void x86_jit_fork_lock(bool take) {
  if (take) {
    g.mu.lock();
    return;
  }
  g.mu.unlock();
  pthread_jit_write_protect_np(1);  // see ppc_jit_fork_lock
}

X86Stop x86_run(X86State& s, u64 max_insns) {
  if (!enabled() || s.trace) return x86_interp(s, max_insns);

  pthread_jit_write_protect_np(1);
  s.jit_budget = (s64)max_insns;
  auto used = [&] { return max_insns - (u64)(s.jit_budget > 0 ? s.jit_budget : 0); };
  // Translated SSE arithmetic is the host's, so the host rounds the way the
  // guest has asked for while it runs, and its own way again afterwards.
  struct Rounding {
    explicit Rounding(u32 mxcsr) { sse_apply_rounding(mxcsr); }
    ~Rounding() { host_rounding(FE_TONEAREST); }
  } rounding(s.mxcsr);
  for (;;) {
    if (s.jit_budget <= 0) {
      s.icount += max_insns;
      return X86Stop{X86StopKind::Budget, s.eip, 0, nullptr};
    }
    u8* const code = code_for(s);
    if (!code) {  // no room for more translations: interpret a while
      const u64 n = s.jit_budget < 256 ? (u64)s.jit_budget : 256;
      const u64 before = s.icount;
      const X86Stop st = x86_interp(s, n);
      s.jit_budget -= (s64)(s.icount - before);
      s.icount = before;
      if (st.kind != X86StopKind::Budget) {
        s.icount += used();
        return st;
      }
      sse_apply_rounding(s.mxcsr);
      continue;
    }
    s.jit_table = g.table;
    s.jit_base = g.base;
    const u64 reason = ((u64 (*)(X86State*, void*))(g.base + g.t_enter))(&s, code);
    flush_flags(&s);
    switch ((Reason)reason) {
      case kLookup:
        break;
      case kBudget:
        s.icount += max_insns;
        return X86Stop{X86StopKind::Budget, s.eip, 0, nullptr};
      case kLink:
        link(s, s.jit_arg);
        break;
      case kStop:
        s.icount += used();
        return tl_stop;
      case kStale:
        x86_jit_invalidate(s.eip, 8);
        break;
    }
  }
}

void x86_jit_report() {
  if (!g_counting) return;
  std::vector<std::pair<u64, u32>> order;
  {
    std::lock_guard<std::mutex> lk(g_count_mu);
    for (const auto& e : g_stepped) order.push_back({e.second, e.first});
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 24; i++) {
    std::fprintf(stderr, "translator: %10llu by the interpreter: bytes %06x\n", (unsigned long long)order[i].first,
                 order[i].second);
  }
}
