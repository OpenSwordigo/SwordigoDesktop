#include "platform/trampoline_mgr.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    constexpr uint64_t target = 0x1000;
    constexpr uint64_t replacement = 0x2000;
    constexpr uint64_t orig_slot = 0x3000;
    constexpr uint32_t original_insn = 0xa9bf7bfd; // stp x29, x30, [sp, #-16]!

    std::vector<uint8_t> memory(TrampolineMgr::ARENA_END, 0);
    *reinterpret_cast<uint32_t*>(memory.data() + target) = original_insn;

    auto& mgr = TrampolineMgr::instance();
    mgr.init(memory.data(), 0);
    assert(mgr.install_hook("relay_order_regression", target, replacement, orig_slot));

    const uint64_t cave = *reinterpret_cast<uint64_t*>(memory.data() + orig_slot);
    assert(cave == TrampolineMgr::ARENA_BASE);
    assert(*reinterpret_cast<uint32_t*>(memory.data() + cave) == original_insn);
    assert(*reinterpret_cast<uint32_t*>(memory.data() + target) != original_insn);
    assert(*reinterpret_cast<uint32_t*>(memory.data() + cave) != 0xd4400000u);

    assert(mgr.uninstall_hook(target));
    assert(*reinterpret_cast<uint32_t*>(memory.data() + target) == original_insn);
    return 0;
}
