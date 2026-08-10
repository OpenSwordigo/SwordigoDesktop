#ifndef UNICORN_DYN_H
#define UNICORN_DYN_H

/* ============================================================================
 * unicorn_dyn.h — Runtime-loaded Unicorn Engine (optional backend).
 *
 * Unicorn is deliberately NOT a build-time dependency. All the uc_* functions
 * used by the emulator backends are resolved at runtime from libunicorn.so
 * (Linux: app dir -> system paths) or unicorn.dll (Windows: app dir -> PATH).
 *
 * If the library is not present, unicorn_available() returns false and the
 * launcher prompts the user (drop libunicorn.so / unicorn.dll next to the app,
 * or install the system package). This is what lets a single binary ship on
 * both Windows and Linux without forcing the Unicorn dev package on anyone.
 *
 * The enum values below mirror the public Unicorn ABI (stable across 1.x/2.x)
 * and were copied from unicorn/arm.h, unicorn/arm64.h and unicorn/unicorn.h.
 * ============================================================================ */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Minimal public types (ABI-compatible) ---- */
struct uc_struct;
typedef struct uc_struct uc_engine;
typedef size_t uc_hook;
typedef int uc_err;
typedef int uc_arch;
typedef int uc_mode;
typedef int uc_prot;
typedef int uc_mem_type;

/* ---- uc_arch ---- */
#define UC_ARCH_ARM 1
#define UC_ARCH_ARM64 2

/* ---- uc_mode ---- */
#define UC_MODE_ARM 0

/* ---- uc_prot ---- */
#define UC_PROT_NONE 0
#define UC_PROT_READ 1
#define UC_PROT_WRITE 2
#define UC_PROT_EXEC 4
#define UC_PROT_ALL 7

/* ---- uc_mem_type (subset used by callbacks) ---- */
#define UC_MEM_READ 16
#define UC_MEM_WRITE 17
#define UC_MEM_FETCH 18
#define UC_MEM_READ_UNMAPPED 19
#define UC_MEM_WRITE_UNMAPPED 20
#define UC_MEM_FETCH_UNMAPPED 21
#define UC_MEM_WRITE_PROT 22
#define UC_MEM_READ_PROT 23
#define UC_MEM_FETCH_PROT 24

/* ---- uc_err ---- */
#define UC_ERR_OK 0
#define UC_ERR_NOMEM 1
#define UC_ERR_ARCH 2
#define UC_ERR_HANDLE 3
#define UC_ERR_MODE 4
#define UC_ERR_VERSION 5
#define UC_ERR_READ_UNMAPPED 6
#define UC_ERR_WRITE_UNMAPPED 7
#define UC_ERR_FETCH_UNMAPPED 8
#define UC_ERR_HOOK 9
#define UC_ERR_INSN_INVALID 10
#define UC_ERR_MAP 11
#define UC_ERR_WRITE_PROT 12
#define UC_ERR_READ_PROT 13
#define UC_ERR_FETCH_PROT 14
#define UC_ERR_ARG 15
#define UC_ERR_READ_UNALIGNED 16
#define UC_ERR_WRITE_UNALIGNED 17
#define UC_ERR_FETCH_UNALIGNED 18
#define UC_ERR_HOOK_EXIST 19
#define UC_ERR_RESOURCE 20
#define UC_ERR_EXCEPTION 21
#define UC_ERR_OVERFLOW 22

/* ---- uc_hook types ---- */
#define UC_HOOK_INTR (1 << 0)
#define UC_HOOK_INSN (1 << 1)
#define UC_HOOK_CODE (1 << 2)
#define UC_HOOK_BLOCK (1 << 3)
#define UC_HOOK_MEM_READ_UNMAPPED (1 << 4)
#define UC_HOOK_MEM_WRITE_UNMAPPED (1 << 5)
#define UC_HOOK_MEM_FETCH_UNMAPPED (1 << 6)
#define UC_HOOK_MEM_READ_PROT (1 << 7)
#define UC_HOOK_MEM_WRITE_PROT (1 << 8)
#define UC_HOOK_MEM_FETCH_PROT (1 << 9)
#define UC_HOOK_MEM_READ (1 << 10)
#define UC_HOOK_MEM_WRITE (1 << 11)
#define UC_HOOK_MEM_FETCH (1 << 12)
#define UC_HOOK_MEM_READ_AFTER (1 << 13)
#define UC_HOOK_INSN_INVALID (1 << 14)
#define UC_HOOK_EDGE_GENERATED (1 << 15)
#define UC_HOOK_TCG_OPCODE (1 << 16)
#define UC_HOOK_TLB_FILL (1 << 17)
#define UC_HOOK_MEM_UNMAPPED \
    (UC_HOOK_MEM_READ_UNMAPPED + UC_HOOK_MEM_WRITE_UNMAPPED + UC_HOOK_MEM_FETCH_UNMAPPED)

/* ---- ARM32 registers (mirrors unicorn/arm.h, ABI-stable) ----
 * Sequential values are load-bearing: the emulators do UC_ARM_REG_R0 + reg,
 * UC_ARM_REG_S0 + i and UC_ARM_REG_D0 + i. */
enum uc_arm_reg {
    UC_ARM_REG_INVALID = 0,
    UC_ARM_REG_APSR,
    UC_ARM_REG_APSR_NZCV,
    UC_ARM_REG_CPSR,
    UC_ARM_REG_FPEXC,
    UC_ARM_REG_FPINST,
    UC_ARM_REG_FPSCR,
    UC_ARM_REG_FPSCR_NZCV,
    UC_ARM_REG_FPSID,
    UC_ARM_REG_ITSTATE,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_SP,
    UC_ARM_REG_SPSR,
    UC_ARM_REG_D0,
    UC_ARM_REG_D1,
    UC_ARM_REG_D2,
    UC_ARM_REG_D3,
    UC_ARM_REG_D4,
    UC_ARM_REG_D5,
    UC_ARM_REG_D6,
    UC_ARM_REG_D7,
    UC_ARM_REG_D8,
    UC_ARM_REG_D9,
    UC_ARM_REG_D10,
    UC_ARM_REG_D11,
    UC_ARM_REG_D12,
    UC_ARM_REG_D13,
    UC_ARM_REG_D14,
    UC_ARM_REG_D15,
    UC_ARM_REG_D16,
    UC_ARM_REG_D17,
    UC_ARM_REG_D18,
    UC_ARM_REG_D19,
    UC_ARM_REG_D20,
    UC_ARM_REG_D21,
    UC_ARM_REG_D22,
    UC_ARM_REG_D23,
    UC_ARM_REG_D24,
    UC_ARM_REG_D25,
    UC_ARM_REG_D26,
    UC_ARM_REG_D27,
    UC_ARM_REG_D28,
    UC_ARM_REG_D29,
    UC_ARM_REG_D30,
    UC_ARM_REG_D31,
    UC_ARM_REG_FPINST2,
    UC_ARM_REG_MVFR0,
    UC_ARM_REG_MVFR1,
    UC_ARM_REG_MVFR2,
    UC_ARM_REG_Q0,
    UC_ARM_REG_Q1,
    UC_ARM_REG_Q2,
    UC_ARM_REG_Q3,
    UC_ARM_REG_Q4,
    UC_ARM_REG_Q5,
    UC_ARM_REG_Q6,
    UC_ARM_REG_Q7,
    UC_ARM_REG_Q8,
    UC_ARM_REG_Q9,
    UC_ARM_REG_Q10,
    UC_ARM_REG_Q11,
    UC_ARM_REG_Q12,
    UC_ARM_REG_Q13,
    UC_ARM_REG_Q14,
    UC_ARM_REG_Q15,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_R12,
    UC_ARM_REG_S0,
    UC_ARM_REG_S1,
    UC_ARM_REG_S2,
    UC_ARM_REG_S3,
    UC_ARM_REG_S4,
    UC_ARM_REG_S5,
    UC_ARM_REG_S6,
    UC_ARM_REG_S7,
    UC_ARM_REG_S8,
    UC_ARM_REG_S9,
    UC_ARM_REG_S10,
    UC_ARM_REG_S11,
    UC_ARM_REG_S12,
    UC_ARM_REG_S13,
    UC_ARM_REG_S14,
    UC_ARM_REG_S15,
    UC_ARM_REG_S16,
    UC_ARM_REG_S17,
    UC_ARM_REG_S18,
    UC_ARM_REG_S19,
    UC_ARM_REG_S20,
    UC_ARM_REG_S21,
    UC_ARM_REG_S22,
    UC_ARM_REG_S23,
    UC_ARM_REG_S24,
    UC_ARM_REG_S25,
    UC_ARM_REG_S26,
    UC_ARM_REG_S27,
    UC_ARM_REG_S28,
    UC_ARM_REG_S29,
    UC_ARM_REG_S30,
    UC_ARM_REG_S31,
    UC_ARM_REG_C1_C0_2,
    UC_ARM_REG_C13_C0_2,
    UC_ARM_REG_C13_C0_3,
};

/* ---- ARM64 registers (mirrors unicorn/arm64.h, ABI-stable) ----
 * Sequential values are load-bearing: the emulators do UC_ARM64_REG_X0 + reg,
 * UC_ARM64_REG_D0 + reg and UC_ARM64_REG_S0 + reg. */
enum uc_arm64_reg {
    UC_ARM64_REG_INVALID = 0,
    UC_ARM64_REG_X29,
    UC_ARM64_REG_X30,
    UC_ARM64_REG_NZCV,
    UC_ARM64_REG_SP,
    UC_ARM64_REG_WSP,
    UC_ARM64_REG_WZR,
    UC_ARM64_REG_XZR,
    UC_ARM64_REG_B0,
    UC_ARM64_REG_B1,
    UC_ARM64_REG_B2,
    UC_ARM64_REG_B3,
    UC_ARM64_REG_B4,
    UC_ARM64_REG_B5,
    UC_ARM64_REG_B6,
    UC_ARM64_REG_B7,
    UC_ARM64_REG_B8,
    UC_ARM64_REG_B9,
    UC_ARM64_REG_B10,
    UC_ARM64_REG_B11,
    UC_ARM64_REG_B12,
    UC_ARM64_REG_B13,
    UC_ARM64_REG_B14,
    UC_ARM64_REG_B15,
    UC_ARM64_REG_B16,
    UC_ARM64_REG_B17,
    UC_ARM64_REG_B18,
    UC_ARM64_REG_B19,
    UC_ARM64_REG_B20,
    UC_ARM64_REG_B21,
    UC_ARM64_REG_B22,
    UC_ARM64_REG_B23,
    UC_ARM64_REG_B24,
    UC_ARM64_REG_B25,
    UC_ARM64_REG_B26,
    UC_ARM64_REG_B27,
    UC_ARM64_REG_B28,
    UC_ARM64_REG_B29,
    UC_ARM64_REG_B30,
    UC_ARM64_REG_B31,
    UC_ARM64_REG_D0,
    UC_ARM64_REG_D1,
    UC_ARM64_REG_D2,
    UC_ARM64_REG_D3,
    UC_ARM64_REG_D4,
    UC_ARM64_REG_D5,
    UC_ARM64_REG_D6,
    UC_ARM64_REG_D7,
    UC_ARM64_REG_D8,
    UC_ARM64_REG_D9,
    UC_ARM64_REG_D10,
    UC_ARM64_REG_D11,
    UC_ARM64_REG_D12,
    UC_ARM64_REG_D13,
    UC_ARM64_REG_D14,
    UC_ARM64_REG_D15,
    UC_ARM64_REG_D16,
    UC_ARM64_REG_D17,
    UC_ARM64_REG_D18,
    UC_ARM64_REG_D19,
    UC_ARM64_REG_D20,
    UC_ARM64_REG_D21,
    UC_ARM64_REG_D22,
    UC_ARM64_REG_D23,
    UC_ARM64_REG_D24,
    UC_ARM64_REG_D25,
    UC_ARM64_REG_D26,
    UC_ARM64_REG_D27,
    UC_ARM64_REG_D28,
    UC_ARM64_REG_D29,
    UC_ARM64_REG_D30,
    UC_ARM64_REG_D31,
    UC_ARM64_REG_H0,
    UC_ARM64_REG_H1,
    UC_ARM64_REG_H2,
    UC_ARM64_REG_H3,
    UC_ARM64_REG_H4,
    UC_ARM64_REG_H5,
    UC_ARM64_REG_H6,
    UC_ARM64_REG_H7,
    UC_ARM64_REG_H8,
    UC_ARM64_REG_H9,
    UC_ARM64_REG_H10,
    UC_ARM64_REG_H11,
    UC_ARM64_REG_H12,
    UC_ARM64_REG_H13,
    UC_ARM64_REG_H14,
    UC_ARM64_REG_H15,
    UC_ARM64_REG_H16,
    UC_ARM64_REG_H17,
    UC_ARM64_REG_H18,
    UC_ARM64_REG_H19,
    UC_ARM64_REG_H20,
    UC_ARM64_REG_H21,
    UC_ARM64_REG_H22,
    UC_ARM64_REG_H23,
    UC_ARM64_REG_H24,
    UC_ARM64_REG_H25,
    UC_ARM64_REG_H26,
    UC_ARM64_REG_H27,
    UC_ARM64_REG_H28,
    UC_ARM64_REG_H29,
    UC_ARM64_REG_H30,
    UC_ARM64_REG_H31,
    UC_ARM64_REG_Q0,
    UC_ARM64_REG_Q1,
    UC_ARM64_REG_Q2,
    UC_ARM64_REG_Q3,
    UC_ARM64_REG_Q4,
    UC_ARM64_REG_Q5,
    UC_ARM64_REG_Q6,
    UC_ARM64_REG_Q7,
    UC_ARM64_REG_Q8,
    UC_ARM64_REG_Q9,
    UC_ARM64_REG_Q10,
    UC_ARM64_REG_Q11,
    UC_ARM64_REG_Q12,
    UC_ARM64_REG_Q13,
    UC_ARM64_REG_Q14,
    UC_ARM64_REG_Q15,
    UC_ARM64_REG_Q16,
    UC_ARM64_REG_Q17,
    UC_ARM64_REG_Q18,
    UC_ARM64_REG_Q19,
    UC_ARM64_REG_Q20,
    UC_ARM64_REG_Q21,
    UC_ARM64_REG_Q22,
    UC_ARM64_REG_Q23,
    UC_ARM64_REG_Q24,
    UC_ARM64_REG_Q25,
    UC_ARM64_REG_Q26,
    UC_ARM64_REG_Q27,
    UC_ARM64_REG_Q28,
    UC_ARM64_REG_Q29,
    UC_ARM64_REG_Q30,
    UC_ARM64_REG_Q31,
    UC_ARM64_REG_S0,
    UC_ARM64_REG_S1,
    UC_ARM64_REG_S2,
    UC_ARM64_REG_S3,
    UC_ARM64_REG_S4,
    UC_ARM64_REG_S5,
    UC_ARM64_REG_S6,
    UC_ARM64_REG_S7,
    UC_ARM64_REG_S8,
    UC_ARM64_REG_S9,
    UC_ARM64_REG_S10,
    UC_ARM64_REG_S11,
    UC_ARM64_REG_S12,
    UC_ARM64_REG_S13,
    UC_ARM64_REG_S14,
    UC_ARM64_REG_S15,
    UC_ARM64_REG_S16,
    UC_ARM64_REG_S17,
    UC_ARM64_REG_S18,
    UC_ARM64_REG_S19,
    UC_ARM64_REG_S20,
    UC_ARM64_REG_S21,
    UC_ARM64_REG_S22,
    UC_ARM64_REG_S23,
    UC_ARM64_REG_S24,
    UC_ARM64_REG_S25,
    UC_ARM64_REG_S26,
    UC_ARM64_REG_S27,
    UC_ARM64_REG_S28,
    UC_ARM64_REG_S29,
    UC_ARM64_REG_S30,
    UC_ARM64_REG_S31,
    UC_ARM64_REG_W0,
    UC_ARM64_REG_W1,
    UC_ARM64_REG_W2,
    UC_ARM64_REG_W3,
    UC_ARM64_REG_W4,
    UC_ARM64_REG_W5,
    UC_ARM64_REG_W6,
    UC_ARM64_REG_W7,
    UC_ARM64_REG_W8,
    UC_ARM64_REG_W9,
    UC_ARM64_REG_W10,
    UC_ARM64_REG_W11,
    UC_ARM64_REG_W12,
    UC_ARM64_REG_W13,
    UC_ARM64_REG_W14,
    UC_ARM64_REG_W15,
    UC_ARM64_REG_W16,
    UC_ARM64_REG_W17,
    UC_ARM64_REG_W18,
    UC_ARM64_REG_W19,
    UC_ARM64_REG_W20,
    UC_ARM64_REG_W21,
    UC_ARM64_REG_W22,
    UC_ARM64_REG_W23,
    UC_ARM64_REG_W24,
    UC_ARM64_REG_W25,
    UC_ARM64_REG_W26,
    UC_ARM64_REG_W27,
    UC_ARM64_REG_W28,
    UC_ARM64_REG_W29,
    UC_ARM64_REG_W30,
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X2,
    UC_ARM64_REG_X3,
    UC_ARM64_REG_X4,
    UC_ARM64_REG_X5,
    UC_ARM64_REG_X6,
    UC_ARM64_REG_X7,
    UC_ARM64_REG_X8,
    UC_ARM64_REG_X9,
    UC_ARM64_REG_X10,
    UC_ARM64_REG_X11,
    UC_ARM64_REG_X12,
    UC_ARM64_REG_X13,
    UC_ARM64_REG_X14,
    UC_ARM64_REG_X15,
    UC_ARM64_REG_X16,
    UC_ARM64_REG_X17,
    UC_ARM64_REG_X18,
    UC_ARM64_REG_X19,
    UC_ARM64_REG_X20,
    UC_ARM64_REG_X21,
    UC_ARM64_REG_X22,
    UC_ARM64_REG_X23,
    UC_ARM64_REG_X24,
    UC_ARM64_REG_X25,
    UC_ARM64_REG_X26,
    UC_ARM64_REG_X27,
    UC_ARM64_REG_X28,
    UC_ARM64_REG_V0,
    UC_ARM64_REG_V1,
    UC_ARM64_REG_V2,
    UC_ARM64_REG_V3,
    UC_ARM64_REG_V4,
    UC_ARM64_REG_V5,
    UC_ARM64_REG_V6,
    UC_ARM64_REG_V7,
    UC_ARM64_REG_V8,
    UC_ARM64_REG_V9,
    UC_ARM64_REG_V10,
    UC_ARM64_REG_V11,
    UC_ARM64_REG_V12,
    UC_ARM64_REG_V13,
    UC_ARM64_REG_V14,
    UC_ARM64_REG_V15,
    UC_ARM64_REG_V16,
    UC_ARM64_REG_V17,
    UC_ARM64_REG_V18,
    UC_ARM64_REG_V19,
    UC_ARM64_REG_V20,
    UC_ARM64_REG_V21,
    UC_ARM64_REG_V22,
    UC_ARM64_REG_V23,
    UC_ARM64_REG_V24,
    UC_ARM64_REG_V25,
    UC_ARM64_REG_V26,
    UC_ARM64_REG_V27,
    UC_ARM64_REG_V28,
    UC_ARM64_REG_V29,
    UC_ARM64_REG_V30,
    UC_ARM64_REG_V31,
    UC_ARM64_REG_PC,
    UC_ARM64_REG_CPACR_EL1,
    UC_ARM64_REG_TPIDR_EL0,
    UC_ARM64_REG_TPIDRRO_EL0,
    UC_ARM64_REG_TPIDR_EL1,
    UC_ARM64_REG_PSTATE,
};

#define UC_ARM64_REG_FP UC_ARM64_REG_X29
#define UC_ARM64_REG_LR UC_ARM64_REG_X30

/* ---- Runtime function-pointer table ----
 * Member names are prefixed fn_* so the macros below (which map the public
 * uc_* names onto table members) never recurse. */
struct unicorn_api_t {
    unsigned int (*fn_uc_version)(unsigned int* major, unsigned int* minor);
    int (*fn_uc_open)(int arch, int mode, uc_engine** uc);
    int (*fn_uc_close)(uc_engine* uc);
    int (*fn_uc_emu_start)(uc_engine* uc, uint64_t begin, uint64_t until,
                           uint64_t timeout, size_t count);
    int (*fn_uc_emu_stop)(uc_engine* uc);
    int (*fn_uc_mem_map)(uc_engine* uc, uint64_t address, size_t size, uint32_t perms);
    int (*fn_uc_mem_map_ptr)(uc_engine* uc, uint64_t address, size_t size,
                             uint32_t perms, void* ptr);
    int (*fn_uc_mem_protect)(uc_engine* uc, uint64_t address, size_t size, uint32_t perms);
    int (*fn_uc_mem_read)(uc_engine* uc, uint64_t address, void* bytes, size_t size);
    int (*fn_uc_mem_write)(uc_engine* uc, uint64_t address, const void* bytes, size_t size);
    int (*fn_uc_reg_read)(uc_engine* uc, int regid, void* value);
    int (*fn_uc_reg_write)(uc_engine* uc, int regid, const void* value);
    int (*fn_uc_hook_add)(uc_engine* uc, uc_hook* hh, int type, void* callback,
                          void* user_data, ...);
    const char* (*fn_uc_strerror)(int code);
};

extern unicorn_api_t unicorn_api;

/* Detect + load libunicorn.so / unicorn.dll. Safe to call repeatedly; the
 * result is cached. Returns true once the table is fully populated. */
int unicorn_backend_available(void);

/* Human-readable reason for diagnostics (English; embedded in the table). */
const char* unicorn_backend_error(void);

#ifdef __cplusplus
}
#endif

/* Route the public Unicorn API to the runtime table. uc_* call sites in the
 * emulator backends stay untouched — only the #include changes. */
#define uc_version  unicorn_api.fn_uc_version
#define uc_open     unicorn_api.fn_uc_open
#define uc_close    unicorn_api.fn_uc_close
#define uc_emu_start unicorn_api.fn_uc_emu_start
#define uc_emu_stop  unicorn_api.fn_uc_emu_stop
#define uc_mem_map   unicorn_api.fn_uc_mem_map
#define uc_mem_map_ptr unicorn_api.fn_uc_mem_map_ptr
#define uc_mem_protect unicorn_api.fn_uc_mem_protect
#define uc_mem_read  unicorn_api.fn_uc_mem_read
#define uc_mem_write unicorn_api.fn_uc_mem_write
#define uc_reg_read  unicorn_api.fn_uc_reg_read
#define uc_reg_write unicorn_api.fn_uc_reg_write
#define uc_hook_add  unicorn_api.fn_uc_hook_add
#define uc_strerror  unicorn_api.fn_uc_strerror

#endif /* UNICORN_DYN_H */
