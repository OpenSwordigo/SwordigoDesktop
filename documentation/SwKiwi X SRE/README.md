# SwKiwi X SRE Integration Research & Master Plan

**Status**: ✅ Research Complete | Implementation Ready

**Completion Date**: July 4, 2026  
**Target Mod**: RLSwordigo 7.0.0  
**Target Platform**: SRE (PC Swordigo Engine)

---

## Overview

This research folder contains a comprehensive analysis of SwKiwi (Android modloader for Swordigo) and RLSwordigo 7.0 mod, with a detailed 6-phase implementation roadmap for porting both to the PC-based SRE platform.

**Key Finding**: SwKiwi is portable to PC through strategic hook placement and API layer implementation. RLSW is already designed for extensibility and requires minimal changes. Estimated effort: 5-6 weeks for Phase 1-6 implementation.

---

## Documents Included

### 1. **01-SWKIWI-ARCHITECTURE-ANALYSIS.md** (375 lines, 10.5 KB)

**Deep-dive into SwKiwi's architecture and how it extends Swordigo**

What you'll learn:
- Three-phase initialization flow (earlyLoad → midLoad → lateLoad)
- GlossHook framework and function interception mechanism
- Virtual Filesystem (MiniPath) design and benefits
- LNI (Lua-Native Interface) system for Lua-native communication
- Configuration system (TOML parsing)
- Direct comparison between SwKiwi and SRE architecture

**Use case**: Understand what APIs mods depend on and how to replicate them in SRE

**Key sections**:
- Lines 25-45: Three-phase init flow diagram
- Lines 90-150: Hook system deep dive with code examples
- Lines 190-210: Hook points summary table
- Lines 220-250: Component locations reference

---

### 2. **02-RLSWORDIGO-7.0-MOD-ANALYSIS.md** (615 lines, 16.2 KB)

**Reverse-engineered RLSW mod requirements and architecture**

What you'll learn:
- Data persistence model (db.scl + srlz.scl interaction)
- Serialization format with circular reference handling
- Bauble system (8 types, equipment constraints, leveling)
- Input handling (touch detection, double-click logic)
- Physics system overview
- Bootstrap sequence and initialization order
- External API dependencies and their usage patterns

**Use case**: Know exactly what data must be stored, how UI works, and what the mod expects

**Key sections**:
- Lines 50-120: Data persistence flow with state diagram
- Lines 140-200: Serialization engine deep-dive
- Lines 250-350: Bauble system documentation with tables
- Lines 450-510: External API dependencies with usage examples
- Lines 530-580: Boot sequence timeline

---

### 3. **03-COMPREHENSIVE-MASTER-PLAN.md** (881 lines, 23.3 KB)

**The implementation roadmap — ready for execution**

What you'll learn:
- Mini API layer implementation (C functions + Lua bindings with code)
- LNI system design (Lua-to-native bridge architecture)
- 6-phase phased approach with specific timeline and deliverables
- Exact C function signatures to implement
- Hook point requirements with rationale
- Technology stack recommendations with justification
- Complete risk matrix with mitigation strategies
- File checklist and success metrics

**Use case**: The single source of truth for what to build, how to build it, and in what order

**Quick reference**:
- **Phase 1 (1.5 weeks)**: Mini API + LNI + Config → Core foundations
- **Phase 2 (1.5 weeks)**: VFS + Serialization → Data persistence
- **Phase 3 (1 week)**: UI + Buttons → Input handling
- **Phase 4 (0.5 weeks)**: Baubles → Game-specific systems
- **Phase 5 (1 week)**: Integration + Testing → Hook installation + validation
- **Phase 6 (1 week)**: RLSW Port → Full end-to-end testing

**Total effort**: ~6 weeks (30-35 development days)

**Key sections**:
- Lines 40-100: Mini API layer specification with signatures
- Lines 110-160: LNI system design with architecture diagram
- Lines 200-900: 6-phase breakdown with deliverables per phase
- Lines 1050-1100: Hook point requirements table
- Lines 1200-1250: Risk matrix with severity/probability
- Lines 1350-1410: Implementation file checklist

---

### 4. **04-IMPLEMENTATION-CHALLENGES-SOLUTIONS.md** (654 lines, 17 KB)

**Technical deep-dive on specific problems and recommended solutions**

What you'll learn:
- 10 major implementation challenges with detailed solutions
- Each challenge includes: Problem → Analysis → Recommended Solution → Code examples → Effort estimate
- Fallback strategies for edge cases and performance concerns
- Performance optimization tips and profiling advice
- Cross-platform compatibility considerations

**Use case**: When implementing Phase 1-6, reference this for specific technical decisions and code patterns

**Challenges covered**:
1. Hook system adaptation (GlossHook → SRE)
2. Virtual filesystem on PC (path translation)
3. Character API implementation (hooks + dual-store)
4. Serialization format (Lua integration)
5. Profile management (multi-save support)
6. UI button implementation (coordinate transforms)
7. Performance optimization (async I/O, profiling)
8. Cross-platform compatibility (paths, filesystems)
9. Mod loading order (dependency resolution)
10. Development workflow (hot-reload, file watcher)

**Summary table**: Effort/Risk/Notes for all 10 challenges at end

---

## Effort Breakdown

| Component | Effort | Risk | Notes |
|-----------|--------|------|-------|
| Mini API layer | 2-3d | Low | C functions + Lua bindings |
| LNI system | 2-3d | Low | Lua-native bridge |
| Configuration | 1-2d | Low | TOML parser |
| VFS translation | 3-4d | Low | Path mapping, platform abstraction |
| Serialization | 1-2d | Low | Reuse decompiled code |
| Character API | 5-6d | Medium | Symbol verification needed |
| UI buttons | 5-6d | Medium | Coordinate transforms, event handling |
| Baubles | 2-3d | Low | Game-specific system |
| Integration | 3-4d | Medium | Hook installation, orchestration |
| Testing | 4-5d | Low | Unit + integration + e2e |
| **Total** | **~30-35d** | **Low-Medium** | **6 weeks estimated** |

---

## Timeline Overview

**Week 1 (Phase 1: 1.5 weeks)**
- Days 1-2: Mini API layer (sre_mini_api.c/h)
- Days 3-4: LNI system (sre_lni.c/h)
- Days 5-6: Config parser (sre_config.c/h)
- Day 7: Unit tests + debugging

**Week 2 (Phase 1 finish + Phase 2: 1.5 weeks)**
- Days 8-9: VFS layer (sre_vfs.c/h)
- Days 10-11: Serialization integration
- Day 12: Testing + adjustments

**Week 3 (Phase 2 finish + Phase 3: 1.5 weeks)**
- Days 13-14: UI/buttons (sre_ui.c/h)
- Days 15-16: Touch input adaptation
- Day 17: UI testing

**Week 4 (Phase 3 finish + Phase 4/5: 1.5 weeks)**
- Days 18-19: Baubles implementation
- Days 20-21: Character API hooks
- Day 22: Integration testing begins

**Week 5 (Phase 5 finish + Phase 6: 1.5 weeks)**
- Days 23-25: Full integration + hook installation
- Days 26-27: RLSW mod loading + end-to-end tests
- Day 28: Performance profiling

**Week 6 (Phase 6 finish: 1 week)**
- Days 29-30: Tuning + bug fixes
- Day 31: Final validation + documentation

---

## Success Criteria

**Phase 1 Completion**:
- [ ] Mini.* Lua API fully implemented and tested
- [ ] LNI.* registration system working end-to-end
- [ ] Configuration loaded from TOML without errors
- [ ] 100% unit test pass rate

**Final (Phase 6) Completion**:
- [ ] RLSW 7.0 mod loads without errors
- [ ] Persisted data survives level reload
- [ ] UI buttons clickable and responsive (<100ms latency)
- [ ] Baubles equippable with correct stat bonuses
- [ ] Frame rate maintained (>30 FPS in gameplay)
- [ ] No memory leaks detected (valgrind clean)
- [ ] All features from RLSW 7.0 working as expected

---

## Key Technical Findings

### SwKiwi Architecture
- **22 Java files** + **181 C/C++ files**
- **Three-phase initialization** is critical for hook placement
- **Virtual Filesystem** provides transparent resource override
- **LNI system** simplifies Lua-native communication
- **GlossHook** framework enables runtime function patching

### RLSW 7.0 Requirements
- **Data**: db.scl (persistence) + srlz.scl (serialization)
- **UI**: touch.scl (input factory) + touchable.scl (button logic)
- **Features**: baubles.lua, physics, inventory system
- **Bootstrap**: db.init() called at level load, before game logic
- **Constraints**: Coin limit 65535, flags are only persistent storage

### Integration Strategy
- **Low risk**: VFS, serialization (reuse decompiled code)
- **Medium risk**: Character API hooks (need symbol verification), input system
- **Low effort**: Configuration, profile management
- **High value**: Mini API enables all mod functionality

---

## Implementation Checklist

### Before Day 1
- [ ] Review Master Plan (#3) sections for Phase 1
- [ ] Review Architecture (#1) for hook system understanding
- [ ] Assign Phase 1 developers (2-3 people recommended)
- [ ] Create git feature branch (e.g., `feature/swkiwi-integration-phase1`)
- [ ] Set up CI/build checks and test harness
- [ ] Create Phase 1 unit test scaffolding
- [ ] Coordinate with team on architecture decisions

### Day 1 of Phase 1
- [ ] Begin sre_mini_api.c/h with Mini.Arch, Mini.GetProfileID
- [ ] Set up compilation + incremental build
- [ ] Set up test runner (CUnit or similar)
- [ ] Get first function working end-to-end (Lua → C → Lua)
- [ ] Commit code with WIP status

---

## Quick Start Guide

**For Architecture Questions**: See document #1 (01-SWKIWI-ARCHITECTURE-ANALYSIS.md)

**For RLSW Specifics**: See document #2 (02-RLSWORDIGO-7.0-MOD-ANALYSIS.md)

**For Implementation Guidance**: See document #3 (03-COMPREHENSIVE-MASTER-PLAN.md)

**For Technical Decisions**: See document #4 (04-IMPLEMENTATION-CHALLENGES-SOLUTIONS.md)

---

## Next Steps

1. **Review** documents #1 and #3 with development team
2. **Decide** on technology choices (TOML library, etc.) from #4
3. **Create** git branch and CI configuration
4. **Set up** build environment and test harness
5. **Begin** Phase 1 implementation per master plan

---

## Technical Constraints & Quirks to Remember

- **Coin limit** must be re-applied on every scene change (max: 65535)
- **Character flags** are the ONLY persistent storage mechanism
- **Touchable** click detection requires per-frame updates (0.0001s interval)
- **Srlz** handles NaN/infinity via special encoding (1/0, -1/0, 0/0)
- **Profile ID** defaults to "default" if not available
- **Bauble levels** follow pattern: `bauble_<name>_<level>` (e.g., bauble_magic_1)
- **db.write()** is blocking I/O; 0.1s interval is reasonable to avoid stuttering

---

## Resources & References

**Analyzed Source Code** (NOT modified):
- `/home/quantumcreeper/SwordigoDesktop/reference/SwKiwi-main/` — Full SwKiwi source
- `/home/quantumcreeper/SwordigoDesktop/reference/rlswscldecoded/` — RLSW decompiled Lua

**Existing SRE Files** (will be extended):
- `src/sre/sre_mini_api.c` — Already contains partial Mini API
- `src/sre/sre_lua.h` — Lua function pointer definitions
- `src/sre/sre.h` — Core SRE types and infrastructure
- `src/sre/sre_init.c` — Hook installation entry point

---

## Document Maintenance Notes

- **Last updated**: July 4, 2026
- **Generated by**: GitHub Copilot CLI - Research & Planning Phase
- **Status**: ✅ Complete and Ready for Implementation
- **Next checkpoint**: After Phase 1 completion, create checkpoint for implementation phase

---

**Start implementing from document #3 (Master Plan), Phase 1 section!**
