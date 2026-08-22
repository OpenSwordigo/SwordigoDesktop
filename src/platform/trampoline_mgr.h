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
//     Java_..._updateApplication (IDA/nm: 0x478ccc, size = 28 bytes / 7 insns)
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
#include <cstring>
#include <vector>
#include <string>

// Implemented in platform/arm64_reloc.cpp — relocates ADRP/ADR/B/BL/B.cond/
// CBZ/CBNZ/TBZ/TBNZ/LDR-literal into a code cave. HARDENED: range-checks every
// relocated form. Returns the number of BYTES successfully relocated
// (num_insns * 4), or -1 if any relocated immediate is out of range / an
// unsupported PC-relative opcode is encountered. On -1 the caller must NOT
// execute the partially-written cave. (Declaration mirrors arm64_reloc.h so
// existing includers of trampoline_mgr.h keep compiling unchanged.)
extern int copy_and_relocate(uint8_t* dest_cave, uint8_t* src_orig,
                              uint64_t cave_vaddr, uint64_t orig_vaddr,
                              int num_insns);

struct TrampolineEntry {
    std::string name;
    uint64_t    target_vaddr;   // guest address that was hooked (0 = reservation)
    uint64_t    replacement;    // guest address of the SRE handler (0 = NOP-out)
    uint64_t    cave_vaddr;     // relay cave address (0 = NOP-out)
    int         insns_saved;
    uint8_t     saved_bytes[64];// original guest bytes overwritten by the patch
    int         patch_bytes;    // number of bytes in saved_bytes actually patched (0 = none)
};

class TrampolineMgr {
public:
    static constexpr uint64_t ARENA_BASE = 0x3000000ULL;
    static constexpr uint64_t ARENA_END  = 0x3100000ULL;
    static constexpr uint64_t SLOT_SIZE  = 64ULL;
    static constexpr int MAX_PATCH_BYTES = 64;

    static TrampolineMgr& instance() {
        static TrampolineMgr s;
        return s;
    }

    void init(uint8_t* guest_mem, uint64_t load_base) {
        // Several installation phases run after SRE initialization. Rebinding the
        // same guest image must not erase the registry or invalidate relays.
        if (m_initialized && m_mem == guest_mem && m_load_base == load_base)
            return;
        m_mem         = guest_mem;
        m_load_base   = load_base;
        m_next_cave   = ARENA_BASE;
        m_initialized = true;
        m_entries.clear();
    }

    // ------------------------------------------------------------------
    // install_hook  —  write a direct-B trampoline at target + relay cave
    //
    // @param insns_to_save   How many 4-byte instructions to relocate into the
    //                        relay cave before the return-jump.  Must be >= 1.
    //                        (Default 1 keeps every existing caller byte-for-byte
    //                        equivalent to the previous behavior.)
    //                        For a short function like updateApplication (7 insns)
    //                        pass 7 so ALL pre-trampoline bytes are relocated and
    //                        the return-jump targets the natural continuation.
    // @param allow_replace   When false (default) a second hook on the same
    //                        target_vaddr is REFUSED (duplicate-hook guard).
    //
    // HARDENING: the relocation is range-checked (copy_and_relocate returns -1
    // on out-of-range / unsupported forms). On failure the cave allocation is
    // rolled back and NO patch is written — the hook aborts cleanly. The first
    // 4 original bytes at target are saved into the entry so uninstall_hook can
    // restore them.
    // ------------------------------------------------------------------
    bool install_hook(const char* name,
                      uint64_t    target_vaddr,
                      uint64_t    replacement,
                      uint64_t    g_orig_guest_addr = 0,
                      int         insns_to_save     = 1,
                      bool        allow_replace     = false)
    {
        if (!m_initialized) { die(name, "init() not called"); return false; }
        if (!target_vaddr || !replacement || (target_vaddr & 3) || (replacement & 3)) {
            fprintf(stderr, "[TrampolineMgr] SKIP %s: null address\n", name);
            return false;
        }
        if (insns_to_save < 1) insns_to_save = 1;
        if (insns_to_save * 4 > MAX_PATCH_BYTES) {
            fprintf(stderr, "[TrampolineMgr] SKIP %s: patch span too large\n", name);
            return false;
        }
        if ((uint64_t)(insns_to_save + 1) * 4 > SLOT_SIZE) {
            fprintf(stderr, "[TrampolineMgr] SKIP %s: relay does not fit cave slot\n", name);
            return false;
        }

        // Never relocate an already-patched entry: that would save the previous B
        // instruction as the "original" and create a chained/corrupt relay.
        if (TrampolineEntry* existing = find_entry(target_vaddr)) {
            if (existing->replacement == replacement) {
                if (g_orig_guest_addr)
                    *(uint64_t*)(m_mem + g_orig_guest_addr) = existing->cave_vaddr;
                fprintf(stdout, "[TrampolineMgr] KEEP  %-44s  target=0x%lx already installed\n",
                        name, target_vaddr);
                return true;
            }
            fprintf(stderr,
                "[TrampolineMgr] SKIP %s: target 0x%lx already has replacement 0x%lx\n",
                name, target_vaddr, existing->replacement);
            return false;
        }
        (void)allow_replace;

        uint64_t cave = alloc_cave(name);
        if (!cave) return false;

        int64_t branch_imm = 0;
        if (!encode_b(replacement, target_vaddr, &branch_imm)) {
            fprintf(stderr, "[TrampolineMgr] ABORT %s: replacement out of B range\n", name);
            m_next_cave -= SLOT_SIZE;
            return false;
        }

        // Snapshot the patch site before writing either the cave or target. This
        // keeps relay construction independent of any later patch/bridge write.
        uint8_t original_bytes[MAX_PATCH_BYTES]{};
        const size_t original_size = (size_t)insns_to_save * 4;
        memcpy(original_bytes, m_mem + target_vaddr, original_size);

        // 1. Relocate the first insns_to_save instructions into the cave. If any
        //    relocated form is out of range / unsupported, ABORT: roll back the
        //    cave allocation and leave guest memory untouched (no half-written patch).
        int relocated = copy_and_relocate(m_mem + cave, original_bytes,
                                          cave, target_vaddr, insns_to_save);
        if (relocated < 0) {
            fprintf(stderr,
                "[TrampolineMgr] ABORT %s: copy_and_relocate failed for target=0x%lx "
                "(insns_to_save=%d) — rolling back cave 0x%lx\n",
                name, target_vaddr, insns_to_save, cave);
            m_next_cave -= SLOT_SIZE;   // roll back the allocation
            return false;
        }

        // 2. Append return-jump back to target_vaddr + insns_to_save*4 (direct branch).
        uint64_t ret_slot = cave + (uint64_t)insns_to_save * 4;
        uint32_t* t = (uint32_t*)(m_mem + ret_slot);
        int64_t ret_offset = (int64_t)(target_vaddr + (uint64_t)insns_to_save * 4) - (int64_t)ret_slot;
        if (!encode_b(target_vaddr + (uint64_t)insns_to_save * 4, ret_slot, &ret_offset)) {
            fprintf(stderr, "[TrampolineMgr] ABORT %s: relay return out of B range\n", name);
            m_next_cave -= SLOT_SIZE;
            return false;
        }
        t[0] = 0x14000000 | ((uint64_t)ret_offset & 0x3FFFFFF);

        // 3. Save the original bytes we are about to overwrite (patch size = 4),
        //    then write the direct-branch trampoline at the original function.
        TrampolineEntry entry{};
        entry.name         = name;
        entry.target_vaddr = target_vaddr;
        entry.replacement  = replacement;
        entry.cave_vaddr   = cave;
        entry.insns_saved  = insns_to_save;
        entry.patch_bytes  = 4;
        memcpy(entry.saved_bytes, original_bytes, 4);

        // 4. Publish the call-through relay before making the replacement
        // reachable. A concurrently executing hook can never observe g_orig=0.
        if (g_orig_guest_addr)
            *(uint64_t*)(m_mem + g_orig_guest_addr) = cave;

        uint32_t* tr = (uint32_t*)(m_mem + target_vaddr);
        tr[0] = 0x14000000 | ((uint64_t)branch_imm & 0x3FFFFFF);

        m_entries.push_back(entry);
        fprintf(stdout,
            "[TrampolineMgr] HOOK  %-44s  target=0x%lx  replacement=0x%lx  relay=0x%lx  (saved %d insn%s)\n",
            name, target_vaddr, replacement, cave, insns_to_save, insns_to_save == 1 ? "" : "s");
        return true;
    }

    // Build a call-through relay without patching the target. The relay and
    // g_orig pointer are published only after relocation and the return branch
    // have both been validated.
    bool install_relay(const char* name, uint64_t target_vaddr,
                       uint64_t g_orig_guest_addr, int insns_to_save = 1) {
        if (!m_initialized) { die(name, "init() not called"); return false; }
        if (!target_vaddr || !g_orig_guest_addr || (target_vaddr & 3) || insns_to_save < 1)
            return false;
        if ((uint64_t)(insns_to_save + 1) * 4 > SLOT_SIZE) return false;

        uint64_t cave = alloc_cave(name);
        if (!cave) return false;

        uint8_t original_bytes[MAX_PATCH_BYTES]{};
        const size_t original_size = (size_t)insns_to_save * 4;
        memcpy(original_bytes, m_mem + target_vaddr, original_size);
        if (copy_and_relocate(m_mem + cave, original_bytes,
                              cave, target_vaddr, insns_to_save) < 0) {
            m_next_cave -= SLOT_SIZE;
            return false;
        }

        uint64_t ret_slot = cave + (uint64_t)insns_to_save * 4;
        int64_t ret_imm = 0;
        if (!encode_b(target_vaddr + (uint64_t)insns_to_save * 4,
                      ret_slot, &ret_imm)) {
            m_next_cave -= SLOT_SIZE;
            return false;
        }
        *(uint32_t*)(m_mem + ret_slot) =
            0x14000000 | ((uint64_t)ret_imm & 0x3FFFFFF);
        *(uint64_t*)(m_mem + g_orig_guest_addr) = cave;
        m_entries.push_back({ name, 0, 0, cave, insns_to_save, {}, 0 });
        fprintf(stdout, "[TrampolineMgr] RELAY %-44s target=0x%lx relay=0x%lx\n",
                name, target_vaddr, cave);
        return true;
    }

    // ------------------------------------------------------------------
    // uninstall_hook  —  restore the original bytes at target_vaddr.
    //
    // Restores the saved bytes overwritten by the B trampoline and removes the
    // registry entry. Returns true if a matching hooked entry was found and
    // restored. NOTE: the relay cave slot is intentionally NOT freed (the arena
    // is a monotonic bump allocator); it simply remains reserved.
    //
    // Callers must invalidate the host JIT cache for [target_vaddr, patch_bytes)
    // AFTER this returns (TrampolineMgr does not own the emulator).
    // ------------------------------------------------------------------
    bool uninstall_hook(uint64_t target_vaddr) {
        if (!m_initialized) { die("uninstall_hook", "init() not called"); return false; }
        TrampolineEntry* e = find_entry(target_vaddr);
        if (!e || e->patch_bytes <= 0) {
            fprintf(stderr, "[TrampolineMgr] uninstall_hook: no restorable hook at 0x%lx\n", target_vaddr);
            return false;
        }
        memcpy(m_mem + target_vaddr, e->saved_bytes, (size_t)e->patch_bytes);
        fprintf(stdout,
            "[TrampolineMgr] UNHOOK %-44s  target=0x%lx  (restored %d bytes; relay cave 0x%lx left reserved)\n",
            e->name.c_str(), target_vaddr, e->patch_bytes, e->cave_vaddr);
        remove_entry(target_vaddr);
        return true;
    }

    // Find a hooked entry by target address (nullptr if none). Only matches real
    // hooks (target_vaddr != 0), not bare cave reservations.
    TrampolineEntry* find_entry(uint64_t target_vaddr) {
        if (!target_vaddr) return nullptr;
        for (auto& e : m_entries)
            if (e.target_vaddr == target_vaddr) return &e;
        return nullptr;
    }

    // NOP-out N instructions; no cave allocated
    bool install_hook_noop(const char* name, uint64_t target_vaddr, int num_insns) {
        if (!m_initialized) { die(name, "init() not called"); return false; }
        if (!target_vaddr || (target_vaddr & 3) || num_insns < 1 || num_insns * 4 > MAX_PATCH_BYTES) {
            fprintf(stderr, "[TrampolineMgr] SKIP NOP %s: invalid span\n", name); return false;
        }
        if (find_entry(target_vaddr)) {
            fprintf(stderr, "[TrampolineMgr] SKIP NOP %s: duplicate target\n", name);
            return false;
        }
        TrampolineEntry entry{};
        entry.name = name;
        entry.target_vaddr = target_vaddr;
        entry.insns_saved = num_insns;
        entry.patch_bytes = num_insns * 4;
        memcpy(entry.saved_bytes, m_mem + target_vaddr, (size_t)entry.patch_bytes);
        uint32_t* code = (uint32_t*)(m_mem + target_vaddr);
        for (int i = 0; i < num_insns; i++) code[i] = 0xD503201F;
        m_entries.push_back(entry);
        fprintf(stdout, "[TrampolineMgr] NOP   %-44s  @ 0x%lx  (%d insns)\n",
                name, target_vaddr, num_insns);
        return true;
    }

    // Allocate a cave slot for hand-rolled relay; returns the address
    uint64_t reserve_cave(const char* reason) {
        if (!m_initialized) { die(reason, "init() not called"); return 0; }
        uint64_t addr = alloc_cave(reason);
        if (addr) m_entries.push_back({ reason, 0, 0, addr, 0, {}, 0 });
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

    static bool encode_b(uint64_t destination, uint64_t source, int64_t* imm_out) {
        if ((destination & 3) || (source & 3)) return false;
        int64_t delta = (int64_t)destination - (int64_t)source;
        if ((delta & 3) || !fits_signed(delta / 4, 26)) return false;
        *imm_out = delta / 4;
        return true;
    }

    static bool fits_signed(int64_t v, int bits) {
        const int64_t lo = -((int64_t)1 << (bits - 1));
        const int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
        return v >= lo && v <= hi;
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

    // Remove the first registry entry matching target_vaddr (does not free cave).
    void remove_entry(uint64_t target_vaddr) {
        for (size_t i = 0; i < m_entries.size(); i++) {
            if (m_entries[i].target_vaddr == target_vaddr) {
                m_entries.erase(m_entries.begin() + i);
                return;
            }
        }
    }

    uint8_t*  m_mem         = nullptr;
    uint64_t  m_load_base   = 0;
    uint64_t  m_next_cave   = ARENA_BASE;
    bool      m_initialized = false;
    std::vector<TrampolineEntry> m_entries;
};
