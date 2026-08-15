// arm64_reloc.h — declaration of the ARM64 instruction relocator.
//
// The definition lives in arm64_reloc.cpp. It is intentionally a tiny,
// dependency-free translation unit so the SAME single definition of
// copy_and_relocate() is reused by:
//   • the main application (src/main.cpp, via swordfare_boot / swemu), and
//   • the standalone hook-backend unit tests (tests/hook_*_tests.cpp)
// without dragging in main.cpp's heavy dependencies (Dynarmic, GL, SDL, …).
//
// Behavior is byte-for-byte identical to the previous inline definition that
// used to live in main.cpp — the body was MOVED here unchanged.
//
// copy_and_relocate — relocate num_insns instructions from src_orig into
// dest_cave, rewriting PC-relative operands (ADR/ADRP/B/BL/B.cond/CBZ/CBNZ/
// TBZ/TBNZ/LDR-literal) so they still reference the same absolute targets.
//
// HARDENING (PATH B): every relocated form is RANGE-CHECKED after the new
// immediate is computed. If a relocated immediate does NOT fit its signed
// field width, we DO NOT emit a masked (corrupt) instruction — we return -1.
//
// Return value:
//   >= 0  number of BYTES successfully relocated (num_insns * 4 on success)
//   -1    a relocated form is out-of-range, OR an unsupported PC-relative
//         opcode was encountered that must never be blindly copied into an
//         executable cave.
//
// Non-PC-relative instructions (including RET / BR / BLR, which use a register
// operand and are position-independent) are copied verbatim.
//
// Plain function, no exceptions. On failure the caller must roll back and NOT
// execute the partially-written cave.
#pragma once
#include <cstdint>

int copy_and_relocate(uint8_t* dest_cave, uint8_t* src_orig,
                      uint64_t cave_vaddr, uint64_t orig_vaddr, int num_insns);
