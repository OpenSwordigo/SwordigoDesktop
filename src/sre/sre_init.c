/*
 * sre_init.c — libsre.so initialization and hook table
 *
 * This file is compiled as part of libsre.so (ARM64).
 * It provides:
 *   1. sre_init() — called by host after loading to set up globals
 *   2. sre_init_lua() — sets up Lua API function pointers
 *   3. sre_hook_table — array of {offset, symbol} pairs read by host
 *      to write trampolines in libswordigo.so
 */

#include "sre.h"
#include "sre_lua.h"
#include "sre_caver.h"
#include "sre_setjmp.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Forward declaration: defined later in this file, called from sre_init() */
void sre_install_exception_safeguard(void);


/* =========================================================================
 * Globals
 * ========================================================================= */

/* Set by sre_init() — base address of libswordigo.so in guest memory.
 * Non-static: accessed by sre_gui.c for computing function pointers. */
uint64_t g_swordigo_base = 0;

/* External: empty sentinel pointer (defined in sre_string.c) */
extern char* g_empty_sentinel;

/* =========================================================================
 * Hook Table
 * =========================================================================
 * The host reads this table after loading libsre.so.
 * For each entry, it writes a trampoline at swordigo_base + target_offset
 * that redirects to the symbol in libsre.so.
 *
 * Offsets are for v1.4.12 ARM64 (from SwMini modloader research).
 * Set target_offset = 0 to mark end of table.
 */

SreHookEntry sre_hook_table[] = {
    /* CppString — eliminate atomic refcounting */
    { 0x566bb8, "sre_CppString_from_char_p" },  /* std::string(const char*) */
    { 0x56918c, "sre_CppString_assign"      },  /* std::string::assign()    */
    { 0x567254, "sre_CppString_append"      },  /* std::string::append()    */
    { 0x565220, "sre_CppString_release"     },  /* std::string destructor   */

    /* AudioSystem::EndAudioInterruptionIfNecessary — iOS audio session resumption.
     * IDA: _ZN5Caver11AudioSystem31EndAudioInterruptionIfNecessaryEv @ 0x47ED50
     *   nm thunk at 0x47ef50 is the non-virtual call path only; vtable dispatch
     *   (CaverShell::Init vtable[10]) goes directly to 0x47ED50, bypassing the thunk.
     * Calls alcMakeContextCurrent inside a canary-guarded frame. TPIDR_EL0 changes on
     * JNI context switch → canary mismatch → __stack_chk_fail on every return.
     * On desktop OpenAL, iOS audio session management is irrelevant. No-op it.
     * Re-enabled: without this hook, CaverShell::Init vtable[10] still enters the
     * canary body and fires __stack_chk_fail every frame. */
    { 0x47ED50, "sre_AudioSystem_EndAudioInterruptionIfNecessary" },

    /* __stack_chk_fail replacement — PLT stub 0x1F62D0.
     * IDA-verified: 0x1F62D0 = .__stack_chk_fail (real sym at 0x6FE6F8).
     * TPIDR_EL0 drifts on JNI context switch → canary mismatch on every return.
     * Pure return-to-LR: all callee-saved regs intact, epilogue runs cleanly.
     * Belt-and-suspenders: covers ANY other function whose canary fires due to
     * the same TPIDR_EL0 drift, not just EndAudioInterruptionIfNecessary. */
    { 0x1F62D0, "sre_stack_chk_fail" },

    /* ProgramState — catch Lua errors instead of aborting */
    { 0, "sre_ProgramState_Execute" },  /* offset resolved dynamically by symbol */
    { 0, "sre_ProgramState_Resume"  },  /* offset resolved dynamically by symbol */
    /* ProgramState::Update — handles timer-based coroutine resume with
     * error recovery. On ARM64 the resume is inlined (unlike ARM32 where
     * Update calls Resume separately). After handling the resume safely,
     * sre_ProgramState_Update calls g_orig_ProgramState_Update (relay stub)
     * for the child ProgramState iteration loop.
     * The relay stub is set up by the host after trampoline installation.
     *
     * DISABLED FOR TESTING — let the native engine handle it to reduce
     * per-frame overhead. Lua resume errors will crash without this. */
     { 0, "sre_ProgramState_Update" },
     // { 0, "sre_ProgramState_destructor" },
     { 0x478ccc, "sre_updateApplication" },

    /* Scene Loading Pipeline Hooks — Safety Filters & Error Recovery */
    /* SceneLoadingView::InitWithGameState — offset resolved via sym_hooks (not yet wired,
     * kept as 0 until we add sym_hooks entry). Scene::FinishLoad, SceneObject::FinishLoad,
     * and SceneObjectGroup::FinishLoad have hardcoded nm offsets so they ARE installed
     * without sym_hooks entries:
     *   Scene::FinishLoad           nm arm64 v1.4.12: 0x4642a8
     *   SceneObject::FinishLoad     nm arm64 v1.4.12: 0x470ec4
     *   SceneObjectGroup::FinishLoad nm arm64 v1.4.12: 0x475498
     * These are the PRIMARY cause of scene-transition freeze:
     *   without the hook, corrupted component vtable[9] calls jump into .dynstr
     *   (PC=0x10771ac) and Dynarmic force-returns to LR inside the original code
     *   instead of our setjmp recovery.
     * SceneObjectGroup::FinishLoad additionally calls ProgramState::Execute on
     * attached Lua scripts immediately during scene load — any script error in the
     * original causes ProgramPanic → abort. Our wrapper catches it via setjmp. */
    { 0,        "sre_SceneLoadingView_InitWithGameState" },
    { 0,        "sre_GameSceneController_InitWithScene"  },
    /* SceneLoadingView::Update AND AnimateIn — NOT hooked.
     *
     * Both functions suffer from the same relay-trampoline bug:
     * the relay's literal-pool continuation slot is filled with
     * 0xd4400000d4400000 (BRK#0 bridge bytes) instead of the correct
     * continuation VA.  Calling g_orig_Xxx() therefore jumps to an
     * invalid address and crashes with Dynarmic NoExecuteFault.
     *
     * Root cause (host relay builder): the relay saves bytes at the
     * patch site AFTER the BRK is installed, capturing bridge bytes.
     * This cannot be fixed from SRE C code.
     *
     * The forced gateway teleports using GotoLevel only; the scene
     * transition completes via BackgroundLoad → GVC+0xE9=1 →
     * GameViewController::Update as normal.
     *
     * nm arm64 v1.4.12 (for reference):
     *   SceneLoadingView::Update    0x43650C
     *   SceneLoadingView::AnimateIn 0x436A54  */
    { 0x4642a8, "sre_Scene_FinishLoad"                   },
    { 0x470ec4, "sre_SceneObject_FinishLoad"             },
    { 0x475498, "sre_SceneObjectGroup_FinishLoad"        },
    { 0, "sre_ReadPODModelFromFile"               },
    { 0, "sre_CPVRTModelPOD_ReadFromMemory"       },


    /* luaD_throw — ROOT of all Lua error handling. Every Lua error goes
     * through luaD_throw(L, errcode). Original calls __cxa_throw or
     * ProgramPanic+exit — both crash in Unicorn. Our replacement uses
     * setjmp/longjmp recovery instead of C++ exceptions. */
    { 0x4eb814, "sre_luaD_throw" },

    /* ProgramPanic — safety net (should never fire now that luaD_throw
     * is hooked, but kept as backup).
     * nm -D libswordigo.so v1.4.12 arm64-v8a: 0x4c0d60 */
    { 0x4c0d60, "sre_ProgramPanic" },

    /* Background rendering — our own sky renderer
     * Addresses verified via: aarch64-linux-gnu-nm -D libswordigo.so | grep BackgroundComponent
     * Note: Draw is const (ZNK mangling) */
    /* GUIView::Update and AchievementsManager::Update — both contain
     * STXR/LDXR loops (boost::shared_ptr refcount) and bad pointer
     * dereferences that branch into the files_dir string area (0x20010).
     * We replace both with safe no-ops. */
    //{ 0, "sre_GUIView_Update"             },  /* GUIView::Update(float) */
    { 0, "sre_AchievementsManager_Update" },  /* AchievementsManager::Update(float) */

    { 0x21ded4, "sre_BackgroundComponent_Draw"             },  /* BackgroundComponent::Draw (const) */
    { 0x2b6760, "sre_RotatingBackgroundComponent_Draw"     },  /* RotatingBackgroundComponent::Draw (const) */
    { 0x2b66f8, "sre_RotatingBackgroundComponent_Update"   },  /* RotatingBackgroundComponent::Update */


    /* Visual effects — DATA EXTRACTION ONLY (hooks disabled).
     * Vanilla rendering now works after STXR patches — let the game draw.
     * Our SRE implementations stay in source for future use:
     *   - Portal proximity sound effects
     *   - Glow-based lighting in FBO composite
     *   - Weapon trail particle enhancements
     * Re-enable by uncommenting when we add passthrough trampolines. */
    /* { 0x2ae884, "sre_PortalEffectComponent_Draw"    }, */  /* Portal swirl */
    /* { 0x2b0e90, "sre_SimpleGlowComponent_Draw"      }, */  /* Glow FX */
    /* { 0x2cb828, "sre_WeaponGlowComponent_Draw"      }, */  /* Weapon glow — needs relay trampoline */
    /* { 0x2ccf30, "sre_WeaponTrailComponent_Draw"     }, */  /* Weapon trail — needs relay trampoline */
    /* ========= FULL GUI STACK HOOKS =========
     * Every GUI class's DrawRect goes through our code.
     * All use relay stubs to call original, so the game
     * works normally while we have total control. */

    /* Root window — top of the render tree */
     { 0x4a28bc, "sre_GUIWindow_DrawRect"    },

    /* Core view classes */
     { 0x49f310, "sre_GUIView_DrawRect"      },  /* base class: iterates subviews */
     { 0x49565c, "sre_GUIButton_DrawRect"    },  /* buttons (title + bg) */
     { 0x497aa0, "sre_GUILabel_DrawRect"     },  /* text labels */
     { 0x497658, "sre_GUIFrameView_DrawRect" },  /* styled frames */

    /* Interactive controls */
     { 0x491b54, "sre_GUIAlertView_DrawRect" },  /* modal dialogs */
     { 0x49cd40, "sre_GUISlider_DrawRect"    },  /* sliders */

    /* Game-specific views */
     { 0x42bae4, "sre_NewMenuView_DrawRect"  },  /* main menu */


    /* Options button — intercept Offers click at the delegate level.
     * When Offers is clicked, ButtonPressed calls the delegate method
     * MainMenuViewDidOpenShop. We hook THAT instead of ButtonPressed
     * to avoid PC-relative relay issues. */
    { 0x36f394, "sre_MainMenuVC_DidOpenShop" },

    /* CreditsVC hooks — DISABLED for v6, Options menu WIP.
     * { 0x38d604, "sre_CreditsVC_LoadView" },
     * { 0x38d904, "sre_CreditsVC_ButtonPressed" }, */

    /* Death/Respawn fix — desktop has no ad SDK, so ShowAdMaybe hangs.
     * Hook it to directly call GameOverViewDidContinue (respawn). */
    { 0x347efc, "sre_GameOverVC_ShowAdMaybe" },

    /* Text Input — fully intercept the text input chain.
     *
     * Layer 1: Hook StartTextInputWithDelegate / StopTextInputWithDelegate
     *          to capture the delegate pointer in our OWN global and clear
     *          DAT_007f3ca8. This prevents the draw cycle from dispatching
     *          through the corrupt ITextInputDelegate vtable.
     *
     * Layer 2: Hook the JNI textInput functions to read/write
     *          GUITextFieldImpl fields directly (no vtable calls).
     *
     * Layer 3: Host maps a safety RET-page at 0x2d6ce4c to catch any
     *          remaining wild vtable jumps from the PRIMARY vtable. */
    { 0x4792ac, "sre_StartTextInputWithDelegate" },  /* Caver::StartTextInputWithDelegate */
    { 0x4793dc, "sre_StopTextInputWithDelegate"  },  /* Caver::StopTextInputWithDelegate  */
    { 0x4790dc, "sre_textInputTextDidChange" },       /* Java_..._textInputTextDidChange   */
    { 0x479290, "sre_textInputDidFinish"     },       /* Java_..._textInputDidFinish       */
    // { 0x478f84, "sre_handleTouchEvent"      },       /* Java_..._handleTouchEvent (Disabled) */

    /* Lua error safety — wraps ALL lua_call with pcall
     * Installed as LATE trampoline in main.cpp (after sre_init_lua) */
     { 0, "sre_lua_call_safe" }, 

    /* C++ exception handling — hook __cxa_throw to prevent broken unwind.
     * When a C++ exception is thrown (e.g. Lua error), the unwind machinery
     * fails because Unicorn can't capture guest registers. Instead of
     * letting it abort(), we longjmp to the nearest recovery point
     * (set by sre_lua_call_safe). */
    { 0x51e108, "sre_cxa_throw" },

    /* Exception frame initializer — sub_58128C at 0x58128C.
     * Prevents the infinite abort() loop: when C++ exceptions are thrown,
     * the EHABI unwind calls sub_5806A8 which uses dl_iterate_phdr to look
     * up EHABI tables for the guest binary. Since our custom loader does NOT
     * register the binary with the system dynamic linker, dl_iterate_phdr
     * always returns 5. sub_58128C then calls abort() on EVERY exception →
     * infinite recovery loop → eventual PC jump into RTTI data string.
     * Our replacement zeros the unwind frame and returns 0 so sub_5818C4
     * can proceed to a single controlled abort (caught by abort-recovery). */
    { 0, "sre_ExceptionFrameInit" },

    /* C++ exception unwinder dispatcher — sub_5818C4 at 0x5818C4.
     * Even after sre_ExceptionFrameInit suppresses the abort inside
     * sub_58128C, sub_5818C4 passes a zeroed frame to sub_5813B4/sub_581474
     * which compute garbage jump targets → PC lands in RTTI data string
     * (observed: 0x10771ac = "_ZN5Caver11CaverShell...").
     * Fix: return a1 immediately (the same as sub_5818C4's success path)
     * so the exception is silently absorbed without any pointer arithmetic
     * on the zeroed/corrupted frame data.
     *
     * NOTE: This hook has 2744 callers — it is the global C++ exception
     * dispatcher. Disabling it causes the abort loop to return immediately. */
    { 0, "sre_UnwindRaiseException" },


    /* MusicPlayer — FULL native replacement.
     * The original uses boost::shared_ptr + C++ exceptions for playlist
     * management, all of which break under Unicorn. Our SRE version
     * writes commands to shared globals, host executes via OpenAL. */
     { 0x4811a0, "sre_PlayMusicWithName"                },  /* PlayMusicWithName(string&, bool) */
     { 0x4814a8, "sre_MusicPlayer_FadeIn"               },  /* FadeIn(float) */
     { 0x4815d8, "sre_MusicPlayer_FadeOut"               },  /* FadeOut(float) */
     { 0x482090, "sre_MusicPlayer_Update"                },  /* Update(float) */
    /* NOTE: SetVolume(0x482064) and SetLooping(0x48206c) are only 8 bytes apart.
     * Our 16-byte trampoline would clobber one from the other. Leave originals —
     * they call MusicPlayerJNI via JNI bridge (safe). SRE Update handles fading.
     *
     * However, AudioSystem::SetMusicVolume (0x47f5f0) is the HIGH-LEVEL setter
     * called by the engine UI. We hook THIS to route slider changes to OpenAL. */
     { 0x47f5f0, "sre_AudioSystem_SetMusicVolume" },
     { 0x481e88, "sre_MusicPlayer_SetEnabled"            },  /* SetEnabled(bool) */
     { 0x481fc0, "sre_MusicPlayer_SetSuspended"          },  /* SetSuspended(bool) */

    /* NOTE: We do NOT hook AddPlaylist (0x48093c) or RegisterProgramLibrary (0x4821c8).
     * RegisterProgramLibrary registers Lua bindings ("MusicPlayer:PlayMusicWithName" etc.)
     * that scripts call. Those bindings invoke the C++ methods which we've already hooked.
     * AddPlaylist populates the playlist list; harmless since we bypass playlists. */

    /* GUI System — game state extraction.
     * Hooks GameSceneView::Update to read HP/mana/coins from GameState
     * every frame. The native HUD won't animate (we own the display). */
     { 0x34ed2c, "sre_GameSceneView_Update" },  /* GameSceneView::Update(float)  nm arm64: 0x34ed2c ✓ */
      { 0, "sre_CameraController_Update" },
      { 0, "sre_SceneGrid_UpdateVisibleAreasWithCamera" },
      { 0, "sre_GameOverlayView_SetControlsHidden" },

     /* =========================================================================
      * Frame Loop Control Trampolines — sre_frame_loop.c
      * =========================================================================
      *
      * CaverShell::Update: PC-safe replacement strips Android-specific
      * calls (AchievementsManager, GUIApplication::DispatchEvents) and routes
      * AudioSystem::Update + GameSceneView vtable dispatch correctly.
      *
      * GameSceneController::Update: Call-through hook. Required so that
      * the time-sliced ProgramState::Update trampoline sits inside the call chain.
      *
      * Scene::Update: Call-through hook so ProgramState budget applies.
      *
      * GUIApplication::DispatchEvents: Pure PC no-op. SDL drives input events
      * before updateApplication; the game's internal event queue is never needed.
      *
      * Offsets verified from nm -D libswordigo.so v1.4.12 arm64-v8a.
      */
     { 0, "sre_CaverShell_Update_trampoline"    },  /* CaverShell::Update(float)        nm arm64: 0x210efc */
     { 0, "sre_GameSceneController_Update"       },  /* GameSceneController::Update(float) nm arm64: 0x349d84 */
     { 0, "sre_Scene_Update"                     },  /* Scene::Update(float)             nm arm64: 0x465968 */

    /* ─── GUINavigationController Safety Hooks ────────────────────────────────
     * These three functions form the level-transition vtable dispatch chain.
     * During scene unload, a ViewController's vtable pointer can be corrupted
     * (freed), causing the CPU to branch into .dynstr string data at 0x10771ac.
     *
     * Addresses verified from nm -D libswordigo.so v1.4.12 arm64-v8a (CORRECT):
     *   GUINavigationController::Update                   0x49923c
     *   GUINavigationController::ViewControllerViewLoaded 0x499288
     *   GUINavigationController::FinishTransitionToVC     0x49a42c
     */
    { 0, "sre_GUINavigationController_Update"           },
    { 0, "sre_GUINavigationController_VCLoaded"         },
    { 0, "sre_GUINavigationController_FinishTransition" },


    /* ─── GameData::Clear crash guard ─────────────────────────────────────────
     * GameData::Clear iterates 5 repeated protobuf fields calling vtable[0x20]
     * on each element. If a count field is corrupted (e.g. 0x1077 = 4215),
     * the loop runs over garbage pointers → PC jumps into .dynstr at 0x10771ac.
     *
     * nm -D libswordigo.so v1.4.12 arm64-v8a:
     *   Caver::Proto::GameData::Clear  0x2e6d60
     */
    { 0x2e6d60, "sre_GameData_Clear" },

    /* ─── Proto::SceneObject::Clear crash guard ────────────────────────────────
     * CONFIRMED CRASH (live log, LR=0x12fe290=Proto::SceneObject D1Ev):
     *   Scene::LoadFromFile → Proto::Scene::~Scene() → ObjectLibrary::Clear()
     *   → ObjectTemplate::Clear() → SceneObject::Clear()
     *   → loop over Component[this+40] via this+32 array, calling vtable[+32]
     *   on each element. If this+40 is corrupt (use-after-free: allocator link
     *   overwrites freed proto memory), the loop walks garbage → vtable =
     *   ARM64 STP instruction bytes → PC = 0x47bfda9034ff4 → NoExecuteFault.
     *
     * IDA 0x2fc030 = Clear impl (nm-verified via IDA index; IDA addresses ==
     * nm VAs for this stripped binary — confirmed against GameData::Clear).
     * Hook the IMPL, not the PLT thunk (0x1fa880), so all callers are covered.
     */
    { 0x2fc030, "sre_Proto_SceneObject_Clear" },

    /* ─── Proto::ObjectLibrary::Clear crash guard ──────────────────────────────
     * One level above SceneObject::Clear in the chain. IDA 0x2fd374 shows 3
     * vtable-dispatch loops (ObjectTemplate list + 2 unknown repeated fields).
     * If any count field there is corrupt, the outer loop crashes before even
     * reaching SceneObject::Clear, bypassing our guard above.
     * Belt-and-suspenders: guard both layers.
     */
    { 0x2fd374, "sre_Proto_ObjectLibrary_Clear" },

    /* ─── SceneObject::ComponentWithInterface safety guard ─────────────────────
     * Filters out corrupted component pointers / bad vtables created by mods,
     * preventing Dynarmic Halt exceptions and jumps into .dynstr data.
     * nm -D libswordigo.so v1.4.12 arm64-v8a: 0x47462c
     */
    { 0x47462c, "sre_SceneObject_ComponentWithInterface" },

    /* ─── ComponentOutletBase::Connect vtable safety guard ────────────────────
     * CRASH (live log): X8 = 0xf9443508d0002308 (corrupted vtable ptr), PC jumps
     * to non-canonical kernel space → Dynarmic NoExecuteFault.
     * Decompiled body dereferences *(_QWORD*)this+8 and *(_QWORD*)this+32 with
     * ZERO validation. When component memory is freed during level unload, `this`
     * carries garbage vtable bytes from heap allocator free-list links.
     * Our hook validates the vtable before any virtual dispatch.
     *
     * nm -D libswordigo.so v1.4.12 arm64-v8a: 0x247c48 ✓ (confirmed in symbols file)
     */
    { 0x247c48, "sre_ComponentOutletBase_Connect" },

    /* Force GLES 2.0 Mode — hook RenderingContext::C1.
     * nm arm64 v1.4.12: 0x48afb0. Relay built by gui_relays[] in main.cpp
     * (runs BEFORE hook patcher → copy_and_relocate captures real insn). */
    { 0x48afb0, "sre_RenderingContext_C1" },

    /* NOTE: Death/Respawn hook at 0x347efc is already defined above
     * (sre_GameOverVC_ShowAdMaybe). Do NOT duplicate — the old
     * sre_ShowAdMaybe name was from the TVPG snapshot and doesn't
     * exist in the current codebase. */

    /* Mod Support — inject Mini/LNI/Components Lua tables.
     * DISABLED: relay stubs crash because original function uses PC-relative
     * instructions (ADRP) that break when relocated to 0x3000000.
     * Mini.* injection is done via sre_lua_call_safe piggyback instead. */
    { 0x4c0f18, "sre_RegisterProgramLibrary", 0 },

    /* Virtual Filesystem — mod asset layering.
     * Re-enabled: sre_FileExistsAtPath now uses real fopen() checks instead
     * of the old "optimistic return 1" stub that broke vanilla asset queries.
     * sre_NewByteBufferFromAndroidAsset also does real fopen/fread loading.
     * Hook offset 0x4b44b8 = Caver::FileExistsAtPath (v1.4.12 ARM64). */
     { 0x4b44b8, "sre_FileExistsAtPath"                    },  /* nm arm64: 0x4b44b8 ✓ */
     { 0x4b4254, "sre_NewByteBufferFromAndroidAsset"        },  /* nm arm64: 0x4b4254 ✓ */
     { 0x5196c4, "sre_PVRTTextureLoadFromPVRBuffer_hook", 0 },  /* nm arm64: 0x5196c4 ✓ */
     { 0, "sre_SetResourcesPath" },
     /* IsAndroidAssetsPath — ARM64 offset NOT YET VERIFIED.
      * README offsets are ARM32/Thumb — 0x084472 was wrong (mid-instruction).
      * Font loading is fixed via bridge_fopen path rewriting instead. */
     { 0, "sre_IsAndroidAssetsPath" },


    /* Unified Lua Interpreter Hooks */
    
    { 0, "lua_pcall" },
    { 0, "lua_resume" },
    { 0, "lua_settop" },
    { 0, "lua_gettop" },
    { 0, "lua_tolstring" },
    { 0, "lua_call" },
    { 0, "lua_pushstring" },
    { 0, "lua_pushcclosure" },
    { 0, "lua_setfield" },
    { 0, "lua_getfield" },
    { 0, "lua_createtable" },
    { 0, "lua_pushnumber" },
    { 0, "lua_pushboolean" },
    { 0, "lua_pushnil" },
    { 0, "lua_tonumber" },
    { 0, "lua_toboolean" },
    { 0, "lua_type" },
    { 0, "luaL_register" },
    { 0, "lua_touserdata" },
    { 0, "lua_pushlightuserdata" },
    { 0, "lua_error" },
    { 0, "lua_pushvalue" },
    { 0, "lua_remove" },
    { 0, "lua_insert" },
    { 0, "lua_replace" },
    { 0, "lua_checkstack" },
    { 0, "lua_rawget" },
    { 0, "lua_rawset" },
    { 0, "lua_rawgeti" },
    { 0, "lua_rawseti" },
    { 0, "lua_next" },
    { 0, "lua_objlen" },
    { 0, "lua_settable" },
    { 0, "lua_gettable" },
    { 0, "lua_isnumber" },
    { 0, "lua_isstring" },
    { 0, "lua_tointeger" },
    { 0, "lua_pushinteger" },
    { 0, "lua_concat" },
    { 0, "lua_pushlstring" },
    { 0, "lua_setmetatable" },
    { 0, "luaL_newstate" },
    { 0, "luaL_loadstring" },
    { 0, "luaL_loadbuffer" },
    { 0, "luaL_loadfile" },
    { 0, "_Z12luaopen_baseP9lua_State" },
    { 0, "luaopen_package" },
    { 0, "luaopen_table" },
    { 0, "luaopen_io" },
    { 0, "luaopen_os" },
    { 0, "_Z14luaopen_stringP9lua_State" },
    { 0, "luaopen_math" },
    { 0, "luaopen_debug" },
    { 0, "luaL_openlibs" },
    { 0, "lua_newthread" },
    { 0, "lua_xmove" },
    { 0, "lua_tothread" },
    { 0, "lua_pushthread" },
    { 0, "lua_status" },
    { 0, "lua_gc" },
    { 0, "lua_close" },
    { 0, "lua_dump" },
    { 0, "lua_atpanic" },
    { 0, "lua_getmetatable" },
    { 0, "lua_rawequal" },
    { 0, "lua_equal" },
    { 0, "lua_lessthan" },
    { 0, "lua_isuserdata" },
    
    /* Sentinel — end of table */
    { 0, 0, 0 }
};

/* Number of entries (excluding sentinel) */
const int sre_hook_count = (sizeof(sre_hook_table) / sizeof(sre_hook_table[0])) - 1;

/* Resolve a guest function address from the hook table by symbol name */
uint64_t sre_get_hook_address(const char* symbol_name) {
    if (!symbol_name) return 0;
    for (int i = 0; i < sre_hook_count; i++) {
        const char* sym = sre_hook_table[i].symbol_name;
        if (sym) {
            int match = 1;
            int j = 0;
            while (sym[j] || symbol_name[j]) {
                if (sym[j] != symbol_name[j]) {
                    match = 0;
                    break;
                }
                j++;
            }
            if (match) {
                return sre_hook_table[i].orig_func;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

/*
 * sre_init — Called by host after loading both .so files
 *
 * @param swordigo_base  Guest virtual address where libswordigo.so is loaded
 * @param empty_bss_off  BSS offset of the empty string sentinel (0x14880 for v1.4.12)
 *
 * The host passes these values from main.cpp after loading both libraries.
 */
void sre_init(uint64_t swordigo_base, uint64_t empty_bss_off) {
    g_swordigo_base = swordigo_base;

    /* Resolve Caver component interfaces and engine helpers */
    sre_caver_init(swordigo_base);

    /* Calculate the guest address of the empty string sentinel.
     * The empty sentinel is in libswordigo.so's BSS at the given offset.
     * Its _Rep has refcount = -1 (static, never free).
     * The data pointer is: base + bss_offset + sizeof(SreStringRep) 
     * because the sentinel stores [_Rep][data] and we want the data part.
     *
     * Actually, the BSS offset might already point to the data portion.
     * SwMini accesses it as: engine_load_bias + BOFF_CPP_STRING_EMPTY_SENTINEL
     * Let's store the base + offset and check at runtime.
     */
    g_empty_sentinel = (char*)(swordigo_base + empty_bss_off);

    /* Fix ITextInputDelegate vtable — entries have unrelocated pointers.
     *
     * The vtable at binary offset 0x7e1688 contains function pointers to
     * the TextInputTextDidChange and TextInputDidFinish thunks. These
     * were never properly relocated by our ELF loader, causing wild jumps
     * (e.g. to 0x2d6ce4c) when the game's drawApplication dispatches
     * through the vtable during text field rendering.
     *
     * Fix: overwrite the vtable entries with pointers to our SRE handlers.
     * This is safe because Unicorn maps all guest memory as RWX.
     *
     * Vtable layout (ITextInputDelegate):
     *   [+0x00] TextInputTextDidChange(out_str, this, text_str)
     *   [+0x08] TextInputDidFinish(this)
     */
    // Disabled under ARM64 to prevent memory corruption (we clear delegate pointer instead)
    // extern void sre_TextInputTextDidChange_vtable(void*, void*, void*);
    // extern void sre_TextInputDidFinish_vtable(void*);
    // uint64_t* itid_vtable = (uint64_t*)(swordigo_base + 0x7e1688);
    // itid_vtable[0] = (uint64_t)&sre_TextInputTextDidChange_vtable;
    // itid_vtable[1] = (uint64_t)&sre_TextInputDidFinish_vtable;

    /* Load mod configuration from mini.toml */
    extern char g_sre_mod_name[128];
    extern char g_sre_mod_version[32];
    extern char g_sre_mod_author[128];
    extern float g_sre_game_speed;
    extern int g_sre_coin_limit;
    
    #include "sre_config.h"
    sre_config_t config;
    int config_ret = sre_config_load_toml("/Assets/mini.toml", &config);
    if (config_ret != 0) {
        config_ret = sre_config_load_toml("/Assets/resources/mini.toml", &config);
    }
    
    /*
    extern char g_sre_vfs_path_assets[512];
    char scl_path[512];
    snprintf(scl_path, sizeof(scl_path), "%s/resources/is.scl", g_sre_vfs_path_assets);
    FILE* f_scl = fopen(scl_path, "rb");
    if (f_scl) {
        fseek(f_scl, 0, SEEK_END);
        long sz = ftell(f_scl);
        fseek(f_scl, 0, SEEK_SET);
        char* buf = (char*)malloc(sz);
        if (buf) {
            fread(buf, 1, sz, f_scl);
            int has_settings = 0, has_armory = 0, has_keybinds = 0, has_inventory = 0, has_keybind_label = 0;
            for (long i = 0; i < sz - 10; i++) {
                if (memcmp(&buf[i], "Settings", 8) == 0) has_settings = 1;
                if (memcmp(&buf[i], "Armory", 6) == 0) has_armory = 1;
                if (memcmp(&buf[i], "Keybinds", 8) == 0) has_keybinds = 1;
                if (memcmp(&buf[i], "Inventory", 9) == 0) has_inventory = 1;
                if (memcmp(&buf[i], "+ Keybind", 9) == 0) has_keybind_label = 1;
            }
            FILE* f_diag = fopen("/home/quantumcreeper/SwordigoDesktop/sre_scan_diagnostic.txt", "w");
            if (f_diag) {
                fprintf(f_diag, "is.scl string scan results:\n");
                fprintf(f_diag, "  Settings: %d | Armory: %d | Keybinds: %d | Inventory: %d | + Keybind: %d\n",
                        has_settings, has_armory, has_keybinds, has_inventory, has_keybind_label);
                fclose(f_diag);
            }
            free(buf);
        }
        fclose(f_scl);
    } else {
        FILE* f_diag = fopen("/home/quantumcreeper/SwordigoDesktop/sre_scan_diagnostic.txt", "w");
        if (f_diag) {
            fprintf(f_diag, "Failed to open is.scl for scanning!\n");
            fclose(f_diag);
        }
    }
    */

    if (config_ret == 0) {
        strncpy(g_sre_mod_name, config.mod_name, sizeof(g_sre_mod_name) - 1);
        g_sre_mod_name[sizeof(g_sre_mod_name) - 1] = '\0';
        
        strncpy(g_sre_mod_version, config.mod_version, sizeof(g_sre_mod_version) - 1);
        g_sre_mod_version[sizeof(g_sre_mod_version) - 1] = '\0';
        
        strncpy(g_sre_mod_author, config.mod_authors, sizeof(g_sre_mod_author) - 1);
        g_sre_mod_author[sizeof(g_sre_mod_author) - 1] = '\0';
        
        g_sre_game_speed = config.engine_speed;
        g_sre_coin_limit = config.coin_limit;
    }

    /* Install C++ exception terminate handler safeguard.
     * Prevents the __verbose_terminate_handler -> abort() crash loop
     * described in crash report 02. Must run after g_swordigo_base is set. */
    sre_install_exception_safeguard();
}

// Host-side bridge function import
extern void sre_PVRTTextureLoadFromPVRBuffer(
    void *param_1, unsigned long param_2, unsigned int *param_3, 
    void *param_4, unsigned int param_5, unsigned int param_6, 
    int *param_7, int *param_8
);

// Guest-side hook function exported to the engine hook table
void sre_PVRTTextureLoadFromPVRBuffer_hook(
    void *param_1, unsigned long param_2, unsigned int *param_3, 
    void *param_4, unsigned int param_5, unsigned int param_6, 
    int *param_7, int *param_8
) {
    sre_PVRTTextureLoadFromPVRBuffer(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
}

/* =========================================================================
 * sre_custom_terminate_handler — intercepts ARM64 __verbose_terminate_handler
 *
 * Root cause (crash report 02):
 *   An uncaught C++ exception in a guest CaverShell/SceneObject component
 *   causes the ARM64 libc++abi unwinder to call
 *     __gnu_cxx::__verbose_terminate_handler (0x015702A8)
 *       → __cxxabiv1::__terminate (0x0151DFEC)
 *       → abort()
 *   Our abort-recovery interceptor catches abort() but the stack canary
 *   value in X8 (0xdeadc0de12345678) causes the unwind to fail, leaving
 *   Dynarmic halted at 0x10771ac.
 *
 * Fix: install this function as std::set_terminate (at 0x151E068) so the
 *   ABI calls US instead of abort(). We longjmp to the nearest SRE recovery
 *   point, letting the game loop continue without crashing.
 *
 * Architecture note: this is a guest ARM64 function pointer — it is called
 *   BY the guest ABI unwinder, not by the host. The host-side SRE code
 *   patches the guest's std::terminate_handler pointer via a direct write
 *   into guest memory (swordigo_base + 0x151E068).
 * ========================================================================= */

void sre_custom_terminate_handler(void) {
    printf("[SRE/Exception] Uncaught C++ exception intercepted in guest engine — recovering...\n");

    /* Longjmp to the innermost SRE recovery point.
     * g_sre_recovery_depth is kept by sre_lua.c's recovery stack. */
    if (g_sre_recovery_depth > 0) {
        int target = g_sre_recovery_depth - 1;
        sre_recovery_entry* entry = &g_sre_recovery_stack[target];
        sre_longjmp(entry->buf, 1);
        /* never reached */
    }

    /* No recovery point — print and soft-exit rather than aborting */
    printf("[SRE/Exception] No recovery point available — soft-exiting to prevent abort loop.\n");
    /* Deliberately DO NOT call abort() — just return.
     * The JIT will see an invalid PC and Dynarmic will halt gracefully. */
}

/**
 * sre_install_exception_safeguard — patch the guest std::terminate_handler.
 *
 * std::set_terminate is at swordigo_base + 0x151E068 (IDA: _ZSt14set_unexpectedPFvvE).
 * The terminate handler slot is a function pointer in .bss that std::terminate reads.
 * We overwrite that pointer with our sre_custom_terminate_handler guest address.
 *
 * Called from sre_init() after sre_caver_init(), when g_swordigo_base is valid.
 */
void sre_install_exception_safeguard(void) {
    if (!g_swordigo_base) return;

    /* The terminate handler stored in .data is at offset 0x7EEBD8 in libswordigo.so.
     * This is the data pointer that __cxxabiv1::__terminate() reads.
     * From IDA: DAT_007EEBD8 = terminate_handler (function pointer).
     *
     * We write our handler's address there directly.
     * Note: sre_custom_terminate_handler is a GUEST function (compiled into
     *   libsre.so), so we need its guest virtual address. */
    extern void sre_custom_terminate_handler(void);
    uint64_t terminate_slot = g_swordigo_base + 0x7EEBD8ULL;
    /* Cast via void* to avoid uintptr_t without stdint.h on older toolchains */
    void* fn_ptr = (void*)sre_custom_terminate_handler;
    uint64_t handler_guest_vaddr;
    __builtin_memcpy(&handler_guest_vaddr, &fn_ptr, sizeof(handler_guest_vaddr));

    /* Write the new terminate handler pointer into guest memory */
    *(uint64_t*)terminate_slot = handler_guest_vaddr;

    printf("[SRE/Exception] Installed custom terminate handler at 0x%llx -> 0x%llx\n",
           (unsigned long long)terminate_slot,
           (unsigned long long)handler_guest_vaddr);
}

/* Module-scoped VTable validator.
 * Vtable arrays in libswordigo.so live in .data.rel.ro:
 *   VMA 0x6b6a80–0x6DBFxx  → guest addr 0x16b6a80–0x16DBFxx (at load_base 0x1000000)
 * Also accept SRE's vtables (libsre.so loaded at 0x2000000):
 *   Roughly 0x2000000–0x2300000
 *
 * Rejects pointers into .text (0x1203e90–0x1584000) and .dynstr
 * (0x1077168–0x1162a8c) which would indicate a corrupted/stale vtable ptr.
 */
int sre_is_valid_vtable_ptr(uint64_t vtable) {
    if (!vtable) return 0;
    if ((vtable & 7) != 0) return 0;  /* vtable arrays are pointer-aligned */

    /* libswordigo.so .data.rel.ro: 0x16b6a80 – 0x16DC000 */
    if (vtable >= 0x16b6a80ULL && vtable < 0x16DC000ULL) return 1;
    /* libswordigo.so .rodata (some vtables spill into here): 0x1583480 – 0x15A2000 */
    if (vtable >= 0x1583480ULL && vtable < 0x15A2000ULL) return 1;
    /* libsre.so data sections (loaded at ~0x2000000) */
    if (vtable >= 0x2000000ULL && vtable < 0x2300000ULL) return 1;

    return 0;
}

/* =========================================================================
 * Render-Frame Recovery Guard  (Crash-Report-03 Fix)
 *
 * Root cause: CaverShell::Render calls C_Matrix4Mul. If the matrix helper
 * returns via a corrupted LR (e.g. 0x12fd3d9 — 1 byte past the end of
 * Caver::Proto::ObjectLibrary::Clear), Dynarmic tries to decode garbage
 * opcode 0x6854ffff at that misaligned address and halts the JIT.
 *
 * Fix: bracket every CaverShell::Render dispatch with sre_render_guard_begin()
 * + sre_render_guard_end(). The emulator's is_valid_exec_pc() validator and
 * the MemoryReadCode BRK-injection path both call:
 *
 *     sre_longjmp(g_sre_render_recovery_jmp, 2)
 *
 * which unwinds back to the sre_setjmp checkpoint set here, skipping the
 * broken render frame without crashing the process.
 *
 * USAGE (host side, in the render hook):
 *
 *     sre_jmp_buf render_buf;
 *     sre_render_guard_begin(&render_buf);
 *     if (sre_setjmp(render_buf) == 0) {
 *         g_emulator_64->call(render_vaddr, {caverShell, renderCtx});
 *     }
 *     sre_render_guard_end();
 *
 * (The SRE render hook in sre_caver.c calls this.)
 * ========================================================================= */

/* Host-visible globals — externed in emulator_dynarmic64.cpp */
int   g_sre_render_recovery_active = 0;
void* g_sre_render_recovery_jmp    = NULL;

/**
 * sre_render_guard_begin — install a render-frame recovery envelope.
 *
 * @param jmp_buf_ptr  Pointer to the caller's sre_jmp_buf (stack-allocated).
 *                     The emulator will longjmp into it with val=2 on bad PC.
 *                     MUST remain on the stack until sre_render_guard_end() is called.
 */
void sre_render_guard_begin(void* jmp_buf_ptr) {
    g_sre_render_recovery_jmp    = jmp_buf_ptr;
    g_sre_render_recovery_active = 1;
}

/**
 * sre_render_guard_end — remove the render-frame recovery envelope.
 * Must be called after the render call returns (or longjmp fires).
 */
void sre_render_guard_end(void) {
    g_sre_render_recovery_active = 0;
    g_sre_render_recovery_jmp    = NULL;
}
