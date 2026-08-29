# Mini* API Buttons — Findings and PC-port recommendations

Date: 2026-06-28

## Summary
Inspected SwKiwi's Java Button implementation (ButtonController) and related docs. Could not find `reference/decompiled` in the workspace — decompiled Swordigo source was not present at that path, so analysis is based on SwKiwi sources and docs found under `reference/SwKiwi  (another sowrdigo modloader)/`.

Files examined
- app/src/main/java/net/itsjustsomedude/swrdg/ButtonController.java
- app/src/main/res/layout/game_button.xml
- README.md and docs/README.md (SwKiwi)


## How SwKiwi implements Mini.* Buttons (key points)
- Buttons are Android `Button` widgets created at runtime and stored in a HashMap keyed by id.
- Positions/dimensions use normalized coordinates (nx, ny, nw, nh) relative to the root view; layout uses the shorter side (getSquareBase) to compute pixel sizes.
- Text size derived from button height (heightPx * 0.35f).
- Uses `runOnUiThread` for all UI mutations and keeps weak refs to MainActivity and root ViewGroup.
- Movable buttons use an `OnTouchListener` that tracks ACTION_DOWN/MOVE/UP, stores touch offsets, updates LayoutParams, and optionally snapbacks to home.
- Several setters exist: setText, setTextColor, setPadding, setBackgroundResource (via resource name), setAlpha, setScaling, setDimensions, setClickable, isPressed/isDragging, etc.
- Background drawables and fonts loaded from assets/resources via Android APIs.


## Identified current flaws (likely causes of "not working" buttons)
1. Movable OnTouchListener consumes all touch events (returns `true`):
   - When `makeMovable` sets the touch listener and returns `true`, it consumes the touch stream and prevents default click handling (unless explicitly forwarded). This frequently makes buttons non-clickable after being made movable.
2. Inconsistent listener return values:
   - `addButton` sets an OnTouchListener that returns `false` (so clicks work), but `makeMovable` replaces or overrides it with a listener that returns `true`. No explicit click/callback plumbing appears to be installed after that replacement.
3. Layout/measurement race conditions:
   - Code uses `root.getWidth()/getHeight()` immediately when adding buttons; if the view isn't measured yet this will be zero causing invalid sizes/positions. No fallback or deferred layout is present.
4. Dependence on Android-only APIs:
   - Uses android.widget.Button, MotionEvent, Typeface from assets, resource lookup via `getResources().getIdentifier`. These are not portable to PC without adaptation.
5. Normalization math may misplace buttons on unusual aspect ratios:
   - Using `getSquareBase()` (shorter side) for sizing but computing margins using full screen width/height can create unexpected layouts on wide/narrow windows.
6. WeakReference + null checks can silently drop button ops:
   - If mainActivityRef/viewRef becomes null, calls silently return without warning; no error propagation to Lua/engine is visible.
7. No explicit Lua callback binding shown:
   - The controller tracks pressed state but doesn't show integration to invoke Lua functions on click (maybe done elsewhere). If missing, buttons visually appear but never trigger game logic.


## Recommendations for a PC port implementation
Goal: implement Mini.* Button API with equivalent behavior (normalized layout, movable, styling, Lua callbacks) but using cross-platform systems.

1. UI layer choice
   - Use ImGui (for debug/tools) or an in-engine overlay using SDL2 + custom textured quads / immediate-mode UI; or integrate a lightweight GUI like nuklear or use native GLFW/SDL windowed GUI + engine texture rendering.
   - For game-accurate appearance, render button quads using the engine's renderer and use a simple input dispatcher for mouse/keyboard/gamepad.

2. Event semantics (fixes for movability vs clicking)
   - Implement unified input handling where a press records pointer down position/time; if pointer moves beyond a small threshold and movability is enabled, start drag. On release, if not dragged (or if drag distance < threshold), treat as a click and invoke Lua callback.
   - Do NOT consume events blindly; propagate them through a simple hit-test stack so other UI or engine input can still receive events if appropriate.

3. Coordinate system & scaling
   - Keep normalized coordinates (0..1) for center position and normalized size relative to a chosen reference resolution (e.g., 1920x1080). Map to window pixels via: px = round(normalized * windowSize).
   - Prefer separate scale axis (scaleX/scaleY) instead of basing size solely on min(screenW, screenH). Allow a configurable reference policy (use min side for mobile, use width for landscape desktop, etc.).

4. Resources & fonts
   - Load button textures and fonts from the game's assets or a mod-friendly folder (MiniPaths equivalent). Provide fallback font when a requested font isn't found.
   - Avoid Android-specific resource lookups; use file-based paths or resource identifiers defined by the engine.

5. API parity
   - Implement the same function set: addButton, removeButton, setPosition, getPosition, setDimensions, setText, setTextScale, setClickable, setHidden, setHiddenAll, makeMovable, isPressed, isDragging, setBackgroundAlpha, setBackgroundResource (mapping resource names), setTextFont.
   - Ensure operations are thread-safe with a main/UI thread queue; avoid silent failures—return error codes or log when called before UI ready.

6. Debugging & diagnostics
   - Add verbose logging for addButton calls, layout sizes, hit-test misses, and event flow (press/drag/click) while debugging.
   - Expose a test scene that spawns buttons with every feature toggled.


## Short checklist for fixing Android-side issues (if keeping mobile parity)
- Ensure `makeMovable` does not permanently block clicks: either detect taps vs drags (threshold), or call `performClick()` on ACTION_UP when appropriate.
- Defer initial layout if root size==0 (post a runnable to run after layout or listen for ViewTreeObserver.onGlobalLayout).
- Provide clear Lua error/log messages if a call fails due to missing refs.
- Add explicit Lua callback binding (if absent) so clicks invoke engine functions.


## Next steps / action items
- If desired, provide a patch implementing the "tap vs drag" fix in ButtonController (small Java change) and add logging for layout sizes.
- If porting to PC, pick the UI approach (ImGui vs textured-quads) and implement normalized coordinate mapping + input dispatcher and Lua binding.
- If access to the decompiled Swordigo code is needed for deeper engine integration, please provide the `reference/decompiled` folder path or upload it; it was not present here.


---
Notes: analysis performed from SwKiwi sources under `reference/SwKiwi  (another sowrdigo modloader)/`. No decompiled Swordigo files were found at `reference/decompiled` in this workspace.
