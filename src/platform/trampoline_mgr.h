// trampoline_mgr.h  —  Swordigo-Specific Trampoline Manager
//
// WHY THIS EXISTS
// ───────────────
// The old system hard-coded code-cave addresses as magic literals (0x3000040,
// 0x3000080, 0x3000300, …) scattered across 30+ separate locations in main.cpp.
// This caused two classes of critical, hard-to-debug bugs:
//
//   BUG 1 — CAVE OVERLAP
//     Two completely independent callers both picked 0x3000300:
//       • gui_relays table:  RenderingContext::RenderingContext(0)   relay
//       • UPDATE_APP_RELAY:  Java_..._updateApplication              relay
//     The second write silently overwrote the first.  When the engine later
//     called RenderingContext::RenderingContext() it executed updateApplication
//     instructions instead, producing corrupt font/GL state and making every
//     GUI label, button and text invisible on the main menu.
//
//   BUG 2 — SHORT-FUNCTION HAZARD
//     Java_..._updateApplication (Ghidra: 0x479aac, size = 28 bytes / 7 insns)
//       void updateApplication(void) {
//           if (DAT_007e9c20 != NULL)
//               (**(code**)(*DAT_007e9c20 + 0x68))();   // vtable+0x68 = CaverShell::Update
//       }
//     Our trampoline overwrites bytes 0-15 with LDR/BR.  The relay was built
//     by saving bytes 0-15 to the cave then jumping to byte 16 of the original
//     — which IS INSIDE the overwritten region, executing garbage every frame.
//     That is the direct cause of the [PERF/Dynarmic] SLOW call 60ms warnings.
//
// DESIGN
// ──────
// TrampolineMgr is a singleton owning a monotonically growing arena from
// 0x3000000 to 0x30FFFFF (1 MB).  Every allocation is padded to 64 bytes.
// All allocations are tracked; overflow prints a fatal error.
//
//   install_hook()      — general hook with relay cave for call-through
//   install_hook_noop() — NOP-out N instructions (no cave allocated)
//   reserve_cave()      — claim a slot for hand-rolled relay content
//   check_conflicts()   — validate no two entries share the same cave
//   dump()              — print full hook registry for diagnostics

#pragma once
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>

// Implemented in main.cpp — relocates ADRP/ADR/B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ
extern void copy_and_relocate(uint8_t* dest_cave, uint8_t* src_orig,
                               uint64_t cave_vaddr, uint64_t orig_vaddr,
                               int num_insns);

struct TrampolineEntry {
    std::string name;
    uint64_t    target_vaddr;   // guest address that was hooked (0 = reservation)
    uint64_t    replacement;    // guest address of the SRE handler (0 = NOP-out)
    uint64_t    cave_vaddr;     // relay cave address (0 = NOP-out)
    int         insns_saved;
};

class TrampolineMgr {
public:
    static constexpr uint64_t ARENA_BASE = 0x3000000ULL;
    static constexpr uint64_t ARENA_END  = 0x3100000ULL;
    static constexpr uint64_t SLOT_SIZE  = 64ULL;

    static TrampolineMgr& instance() {
        static TrampolineMgr s;
        return s;
    }

    void init(uint8_t* guest_mem, uint64_t load_base) {
        m_mem         = guest_mem;
        m_load_base   = load_base;
        m_next_cave   = ARENA_BASE;
        m_initialized = true;
        m_entries.clear();
    }

    // ------------------------------------------------------------------
    // install_hook  —  write LDR/BR trampoline + relay cave
    //
    // @param insns_to_save   How many 4-byte instructions to save before the
    //                        return-jump.  Must be >= 4.
    //                        For updateApplication (7 insns): pass 7 so ALL
    //                        pre-trampoline bytes are properly relocated and
    //                        the return-jump targets byte 28 (the natural RET).
    //                        This fixes the short-function hazard completely.
    // ------------------------------------------------------------------
    bool install_hook(const char* name,
                      uint64_t    target_vaddr,
                      uint64_t    replacement,
                      uint64_t    g_orig_guest_addr = 0,
                      int         insns_to_save     = 4)
    {
        if (!m_initialized) { die(name, "init() not called"); return false; }
        if (insns_to_save < 4) {
            fprintf(stderr, "[TrampolineMgr] ERROR %s: insns_to_save=%d < 4\n",
                    name, insns_to_save);
            return false;
        }
        if (!target_vaddr || !replacement) {
            fprintf(stderr, "[TrampolineMgr] SKIP %s: null address\n", name);
            return false;
        }

        uint64_t cave = alloc_cave(name);
        if (!cave) return false;

        // 1. Relocate original instructions to cave
        copy_and_relocate(m_mem + cave, m_mem + target_vaddr,
                          cave, target_vaddr, insns_to_save);

        // 2. Append return-jump: LDR X16,[PC,#8]; BR X16; .quad ret_target
        uint64_t  ret = target_vaddr + (uint64_t)(insns_to_save * 4);
        uint32_t* t   = (uint32_t*)(m_mem + cave + (uint64_t)(insns_to_save * 4));
        t[0] = 0x58000050; t[1] = 0xD61F0200;
        *(uint64_t*)(t + 2) = ret;

        // 3. Write trampoline at original function (overwrites first 16 bytes)
        uint32_t* tr = (uint32_t*)(m_mem + target_vaddr);
        tr[0] = 0x58000050; tr[1] = 0xD61F0200;
        *(uint64_t*)(tr + 2) = replacement;

        // 4. Set g_orig_* pointer in libsre.so guest memory
        if (g_orig_guest_addr)
            *(uint64_t*)(m_mem + g_orig_guest_addr) = cave;

        m_entries.push_back({ name, target_vaddr, replacement, cave, insns_to_save });
        fprintf(stdout,
            "[TrampolineMgr] HOOK  %-44s  target=0x%lx  replacement=0x%lx  relay=0x%lx\n",
            name, target_vaddr, replacement, cave);
        return true;
    }

    // NOP-out N instructions; no cave allocated
    bool install_hook_noop(const char* name, uint64_t target_vaddr, int num_insns) {
        if (!m_initialized) { die(name, "init() not called"); return false; }
        if (!target_vaddr) { fprintf(stderr, "[TrampolineMgr] SKIP NOP %s: null\n", name); return false; }
        uint32_t* code = (uint32_t*)(m_mem + target_vaddr);
        for (int i = 0; i < num_insns; i++) code[i] = 0xD503201F;
        m_entries.push_back({ name, target_vaddr, 0, 0, num_insns });
        fprintf(stdout, "[TrampolineMgr] NOP   %-44s  @ 0x%lx  (%d insns)\n",
                name, target_vaddr, num_insns);
        return true;
    }

    // Allocate a cave slot for hand-rolled relay; returns the address
    uint64_t reserve_cave(const char* reason) {
        if (!m_initialized) { die(reason, "init() not called"); return 0; }
        uint64_t addr = alloc_cave(reason);
        if (addr) m_entries.push_back({ reason, 0, 0, addr, 0 });
        return addr;
    }

    // Validate no two entries share the same cave address
    bool check_conflicts() const {
        bool ok = true;
        for (size_t i = 0; i < m_entries.size(); i++) {
            if (!m_entries[i].cave_vaddr) continue;
            for (size_t j = i + 1; j < m_entries.size(); j++) {
                if (m_entries[i].cave_vaddr == m_entries[j].cave_vaddr) {
                    fprintf(stderr,
                        "[TrampolineMgr] *** OVERLAP: '%s' vs '%s' at 0x%lx ***\n",
                        m_entries[i].name.c_str(), m_entries[j].name.c_str(),
                        m_entries[i].cave_vaddr);
                    ok = false;
                }
            }
        }
        if (ok)
            fprintf(stdout, "[TrampolineMgr] Conflict check PASSED.\n");
        return ok;
    }

    void dump() const {
        fprintf(stdout,
            "\n[TrampolineMgr] === Hook Registry  (arena 0x%lx–0x%lx, %zu slots) ===\n",
            ARENA_BASE, m_next_cave, m_entries.size());
        for (auto& e : m_entries)
            fprintf(stdout, "  %-48s  target=0x%08lx  cave=0x%08lx\n",
                    e.name.c_str(), e.target_vaddr, e.cave_vaddr);
        fprintf(stdout, "[TrampolineMgr] =============================================\n\n");
    }

    uint64_t next_cave_addr() const { return m_next_cave; }

private:
    TrampolineMgr() = default;

    static void die(const char* ctx, const char* msg) {
        fprintf(stderr, "[TrampolineMgr] FATAL (%s): %s\n", ctx, msg);
    }

    uint64_t alloc_cave(const char* name) {
        uint64_t addr = m_next_cave;
        if (addr + SLOT_SIZE > ARENA_END) {
            fprintf(stderr, "[TrampolineMgr] FATAL: arena exhausted for '%s'\n", name);
            return 0;
        }
        m_next_cave += SLOT_SIZE;
        return addr;
    }

    uint8_t*  m_mem         = nullptr;
    uint64_t  m_load_base   = 0;
    uint64_t  m_next_cave   = ARENA_BASE;
    bool      m_initialized = false;
    std::vector<TrampolineEntry> m_entries;
};
