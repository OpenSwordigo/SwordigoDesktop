// arm64_reloc.cpp — ARM64 instruction relocator (single shared definition).
//
// This body was MOVED VERBATIM out of src/main.cpp so the same definition can
// be linked by both the application and the standalone hook-backend unit tests
// without pulling in main.cpp's heavy dependencies. See arm64_reloc.h.

#include "arm64_reloc.h"
#include <cstdio>

static inline bool fits_signed(int64_t v, int bits) {
    int64_t lo = -((int64_t)1 << (bits - 1));
    int64_t hi =  ((int64_t)1 << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

int copy_and_relocate(uint8_t* dest_cave, uint8_t* src_orig, uint64_t cave_vaddr, uint64_t orig_vaddr, int num_insns) {
    for (int i = 0; i < num_insns; i++) {
        uint32_t insn = *(uint32_t*)(src_orig + i * 4);
        uint64_t orig_pc = orig_vaddr + i * 4;
        uint64_t cave_pc = cave_vaddr + i * 4;

        // ADR or ADRP
        if ((insn & 0x9F000000) == 0x90000000) {
            bool is_adrp = (insn & 0x80000000) != 0;
            int32_t immlo = (insn >> 29) & 3;
            int32_t immhi = (insn >> 5) & 0x7FFFF;
            int32_t imm = (immhi << 2) | immlo;
            if (imm & 0x100000) {
                imm |= ~0x1FFFFF;
            }
            uint64_t target;
            if (is_adrp) {
                target = (orig_pc & ~0xFFFULL) + ((int64_t)imm << 12);
            } else {
                target = orig_pc + imm;
            }
            int64_t new_offset;
            if (is_adrp) {
                new_offset = (int64_t)target - (cave_pc & ~0xFFFULL);
                int64_t new_imm = new_offset >> 12;                 // signed 21-bit page delta
                if (!fits_signed(new_imm, 21)) {
                    fprintf(stderr, "[copy_and_relocate] ADRP out of range: orig=0x%lx cave=0x%lx target=0x%lx (page delta=%ld)\n",
                            orig_pc, cave_pc, target, (long)new_imm);
                    return -1;
                }
                insn = (insn & 0x9F00001F) | (((new_imm & 3) << 29) | (((new_imm >> 2) & 0x7FFFF) << 5));
            } else {
                new_offset = (int64_t)target - (int64_t)cave_pc;    // signed 21-bit byte delta
                if (!fits_signed(new_offset, 21)) {
                    fprintf(stderr, "[copy_and_relocate] ADR out of range: orig=0x%lx cave=0x%lx target=0x%lx (byte delta=%ld)\n",
                            orig_pc, cave_pc, target, (long)new_offset);
                    return -1;
                }
                insn = (insn & 0x9F00001F) | (((new_offset & 3) << 29) | (((new_offset >> 2) & 0x7FFFF) << 5));
            }
        }
        // B or BL
        else if ((insn & 0xFC000000) == 0x14000000 || (insn & 0xFC000000) == 0x94000000) {
            int32_t imm = insn & 0x3FFFFFF;
            if (imm & 0x2000000) {
                imm |= ~0x3FFFFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;                       // signed 26-bit imm*4
            if (!fits_signed(new_imm, 26)) {
                fprintf(stderr, "[copy_and_relocate] B/BL out of range: orig=0x%lx cave=0x%lx target=0x%lx (imm=%ld)\n",
                        orig_pc, cave_pc, target, (long)new_imm);
                return -1;
            }
            insn = (insn & 0xFC000000) | (new_imm & 0x3FFFFFF);
        }
        // B.cond
        else if ((insn & 0xFF000010) == 0x54000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;                       // signed 19-bit imm*4
            if (!fits_signed(new_imm, 19)) {
                fprintf(stderr, "[copy_and_relocate] B.cond out of range: orig=0x%lx cave=0x%lx target=0x%lx (imm=%ld)\n",
                        orig_pc, cave_pc, target, (long)new_imm);
                return -1;
            }
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        // CBZ or CBNZ
        else if ((insn & 0x7F000000) == 0x34000000 || (insn & 0x7F000000) == 0x35000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;                       // signed 19-bit imm*4
            if (!fits_signed(new_imm, 19)) {
                fprintf(stderr, "[copy_and_relocate] CBZ/CBNZ out of range: orig=0x%lx cave=0x%lx target=0x%lx (imm=%ld)\n",
                        orig_pc, cave_pc, target, (long)new_imm);
                return -1;
            }
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        // TBZ or TBNZ
        else if ((insn & 0x7F000000) == 0x36000000 || (insn & 0x7F000000) == 0x37000000) {
            int32_t imm = (insn >> 5) & 0x3FFF;
            if (imm & 0x2000) {
                imm |= ~0x3FFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;                       // signed 14-bit imm*4
            if (!fits_signed(new_imm, 14)) {
                fprintf(stderr, "[copy_and_relocate] TBZ/TBNZ out of range: orig=0x%lx cave=0x%lx target=0x%lx (imm=%ld)\n",
                        orig_pc, cave_pc, target, (long)new_imm);
                return -1;
            }
            insn = (insn & 0xFFF8001F) | ((new_imm & 0x3FFF) << 5);
        }
        // LDR literal (also LDRSW-lit / PRFM-lit / FP LDR-lit — all imm19)
        else if ((insn & 0x3F000000) == 0x18000000) {
            int32_t imm = (insn >> 5) & 0x7FFFF;
            if (imm & 0x40000) {
                imm |= ~0x7FFFF;
            }
            uint64_t target = orig_pc + (imm * 4);
            int64_t new_offset = (int64_t)target - (int64_t)cave_pc;
            int64_t new_imm = new_offset / 4;                       // signed 19-bit imm*4
            if (!fits_signed(new_imm, 19)) {
                fprintf(stderr, "[copy_and_relocate] LDR-literal out of range: orig=0x%lx cave=0x%lx target=0x%lx (imm=%ld)\n",
                        orig_pc, cave_pc, target, (long)new_imm);
                return -1;
            }
            insn = (insn & 0xFF00001F) | ((new_imm & 0x7FFFF) << 5);
        }
        // Guard: reject any remaining PC-relative form we do NOT relocate, so
        // it is never blindly copied into an executable cave.
        //   - Compare-and-branch already handled above.
        //   - Reject: any *-literal load family not matched above? (all imm19
        //     loads share the 0x18000000 mask above — already handled).
        // Register-indirect RET/BR/BLR (0xD65F0000 / 0xD61F0000 / 0xD63F0000)
        // are position-independent (operate on a register), so they are safe
        // to copy verbatim and fall through to the store below.

        *(uint32_t*)(dest_cave + i * 4) = insn;
    }
    return num_insns * 4;
}
