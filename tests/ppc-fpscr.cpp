#include "cpu/ppc/ppc_jit.h"

#include <cstdio>
#include <vector>

static void put(std::vector<u8>& mem, u32 at, u32 insn) {
  for (int i = 0; i < 4; i++) mem[at + i] = (u8)(insn >> (24 - 8 * i));
}

int main() {
  std::vector<u8> mem(0x10000);
  for (u32 fm = 0; fm < 256; fm++) {
    for (u32 rc = 0; rc < 2; rc++) {
      const u32 pc = 0x1000 + (fm * 2 + rc) * 32;
      put(mem, pc, (63u << 26) | (8u << 21) | (583u << 1) | rc);
      put(mem, pc + 4, (63u << 26) | (fm << 17) | (5u << 11) | (711u << 1) | rc);
      put(mem, pc + 8, (63u << 26) | (9u << 21) | (583u << 1) | rc);
      put(mem, pc + 12, (63u << 26) | (10u << 21) | (6u << 16) | (7u << 11) | (21u << 1));
      put(mem, pc + 16, 0x44000002);  // sc
      for (u32 old_mode = 0; old_mode < 4; old_mode++) {
        for (u32 new_mode = 0; new_mode < 4; new_mode++) {
          PPCState initial;
          initial.mem = mem.data();
          initial.pc = pc;
          initial.cr = 0x76543210;
          initial.fpscr = 0xA5C3D0C0 | old_mode;
          initial.fpr[5].u = 0x123456780F96E430ull | new_mode;
          initial.fpr[6].d = 1.0;
          initial.fpr[7].d = 0x1p-53;
          PPCState interpreted = initial, translated = initial;
          const PPCStop expected = ppc_interp(interpreted, 16);
          const PPCStop actual = ppc_run(translated, 16);
          if (expected.kind != PPCStopKind::Syscall || actual.kind != expected.kind ||
              translated.pc != interpreted.pc || translated.cr != interpreted.cr ||
              translated.fpscr != interpreted.fpscr ||
              translated.fpr[8].u != interpreted.fpr[8].u ||
              translated.fpr[9].u != interpreted.fpr[9].u ||
              translated.fpr[10].u != interpreted.fpr[10].u) {
            std::fprintf(stderr, "FPSCR mismatch: mask=%02x rc=%u old=%u new=%u\n",
                         fm, rc, old_mode, new_mode);
            return 1;
          }
        }
      }
    }
  }
  ppc_jit_report();
  return 0;
}
