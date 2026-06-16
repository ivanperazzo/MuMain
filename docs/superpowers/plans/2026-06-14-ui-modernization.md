# UI Modernization & Mobile-Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the MuMain UI measurable, cut wasted CPU from hidden windows, and introduce the thin abstraction seams that let the UI be improved in place AND ported to mobile — without a rewrite.

**Architecture:** *Strangler-fig*, not replace. Keep all ~80 `NewUI*Window` classes working behind four interfaces (`IUiBackend`/`RenderQueue`, `ITextRenderer`, `IUiInput`, `FrameContext`). The same four seams are simultaneously (a) the improve-without-rewrite boundary, (b) the optional future-replace boundary, and (c) the entire mobile-portability surface. This plan delivers **Phase 0 (measure)** and **Phase 1 (cheap wins)** as detailed, executable tasks, and documents **Phases 2–7** at design altitude (to be expanded into tasks when reached).

**Tech Stack:** C++17, SDL3, legacy OpenGL fixed-function (`ZzzOpenglUtil`), header-only `FrameProfiler`. Build: CMake + Ninja (`windows-x86-debug`). No unit-test harness for UI — verification is **build + run + read the in-game `$details` overlay counters** using the existing perf harness (`MU_TEST_CHARS`, `MU_NOVSYNC`).

**Companion docs (read before starting):**
- `docs/ui/UI_MAP.md` — full UI audit (3 systems, hotspots, `file:line`). This plan assumes it.
- `docs/perf/06-ui-text-cache.md` — the H1 (text) lever is owned there; this plan **measures** it (Task 1) and **references** it, does NOT duplicate it.

---

## Strategy in one picture

```
                       ┌─────────────── UiManager (facade, Phase 5) ───────────────┐
  ~80 NewUI*Window ──▶ │  UpdatePass   RenderPass   InputRouter   TextSubsystem    │
   (unchanged)         └──────┬────────────┬─────────────┬─────────────┬──────────┘
                              │            │             │             │
                      FrameContext    IUiBackend      IUiInput     ITextRenderer
                       (dt,dpi)      (RenderQueue)   (InputEvent)  (glyph atlas)
                              │            │             │             │
                       ── these four seams = improve surface = mobile surface ──
                              ▼            ▼             ▼             ▼
   desktop today:        globals    glBegin/GL      MouseX poll    GDI/DIB (Win32)
   mobile later:         dt+scale   GLES/Metal      touch events   FreeType atlas
```

**Decision recorded:** refine + encapsulate. Do NOT replace the framework — the cost is own CPU (text, hidden update), not the framework. See `UI_MAP.md` §8.

---

## Phase / Task overview

| Phase | Tasks here | Risk | Mobile payoff | Status |
|---|---|---|---|---|
| **0 — Measure** | Task 0, Task 1 | nil | none (enables decisions) | detailed below |
| **1 — Cheap wins** | Task 2 | low | none | detailed below |
| 2 — Render seam | (design) | med | backend swappable (GLES) | roadmap §Phase 2 |
| 3 — Text seam | (design, see `perf/06`) | med | unblocks Android/iOS | roadmap §Phase 3 |
| 4 — Input/context seam | (design) | med | touch + DPI | roadmap §Phase 4 |
| 5 — Unify 3→1 facade | (design) | med-high | — | roadmap §Phase 5 |
| 6 — Mobile bring-up | (design) | med | the port | roadmap §Phase 6 |
| 7 — Optional RmlUi screen | (design) | high | data-driven | roadmap §Phase 7 |

Deferred / not worth it now: the 4 per-frame `std::sort` + 3 vector copies in `CNewUIManager` (`UI_MAP.md` H3). At current window count (N≈50–80) this is ~microseconds; only revisit if N grows into the hundreds.

---

## File Structure (Phase 0 + 1)

| File | Responsibility | Action |
|---|---|---|
| `src/source/Core/Utilities/FrameProfiler.h` | per-pass frame timing enum | Modify: add `UI` pass |
| `src/source/Scenes/MainScene.cpp` | main-scene render orchestration | Modify: wrap UI submission in `FRAME_PROFILE(UI)` |
| `src/source/Scenes/SceneManager.cpp` | `$details` overlay text | Modify: add `U:` field + UI stats line |
| `src/source/UI/NewUI/UiStats.h` | per-frame UI counters (header-only, like FrameProfiler) | **Create** |
| `src/source/UI/NewUI/NewUIManager.cpp` | NewUI update/render loop | Modify: counters + ghost trace + hidden-update gate |
| `src/source/UI/NewUI/NewUIManager.h` | NewUI manager decl | Modify: kill-switch extern |
| `src/source/UI/NewUI/NewUIBase.h` | `CNewUIObj` base | Modify: add `WantsHiddenUpdate()` virtual |
| `src/source/UI/Legacy/UIControls.cpp` | GDI text path | Modify: 2 counter increments |

---

### Task 0: Add a UI pass to the frame profiler + overlay field

**Goal:** The existing `$details` overlay shows a `U:` millisecond field measuring total UI submission, alongside the current `T/O/C/I/E`.

**Files:**
- Modify: `src/source/Core/Utilities/FrameProfiler.h:17-30`
- Modify: `src/source/Scenes/MainScene.cpp:530-559`
- Modify: `src/source/Scenes/SceneManager.cpp:570-577`

**Acceptance Criteria:**
- [ ] `FrameProfiler::Pass` has a `UI` slot; `kPassNames` length still equals `(int)Pass::Count_` (compile-time array — mismatched length fails to build).
- [ ] `RenderMainSceneUI()` is wrapped in a `FRAME_PROFILE(UI)` scope.
- [ ] Overlay `Frame ms` line shows `U:%5.2f`, reading `FrameProfiler::AccumulatorMs(FP::UI)`.
- [ ] `U:` reads ~0 with everything closed and rises when inventory/stats are open.

**Verify:** `cmake --build --preset windows-x86-debug`, launch via `I:\MuOnline\launch_client.bat`, enable the `$details` overlay → the `Frame ms` line now ends with `U: x.xx`.

**Steps:**

- [ ] **Step 1: Add the `UI` pass to the enum and names array**

In `src/source/Core/Utilities/FrameProfiler.h`, change the enum and names (keep `Count_` last; keep array length aligned):

```cpp
    enum class Pass : int
    {
        Terrain,
        Objects,
        Characters,
        Items,
        Effects,
        UI,
        Other,
        Count_
    };

    inline constexpr const char* kPassNames[(int)Pass::Count_] = {
        "Terrain", "Objects", "Chars", "Items", "Effects", "UI", "Other"
    };
```

- [ ] **Step 2: Wrap the UI submission in a profile scope**

In `src/source/Scenes/MainScene.cpp`, `RenderMainSceneUI()` (line 530). `MainScene.cpp` already uses `FRAME_PROFILE` (line 418), so the include is present. Add the scope as the first statement:

```cpp
static void RenderMainSceneUI()
{
    FRAME_PROFILE(UI);
    Input::Selection::SelectObjects();
    BeginBitmap();
    RenderObjectDescription();
    // ... rest unchanged ...
}
```

- [ ] **Step 3: Add the `U:` field to the overlay line**

In `src/source/Scenes/SceneManager.cpp` (line 571), extend the format string and argument list:

```cpp
    swprintf(szLine, L"Frame ms  T:%5.2f  O:%5.2f  C:%5.2f  I:%5.2f  E:%5.2f  U:%5.2f",
             FrameProfiler::AccumulatorMs(FP::Terrain),
             FrameProfiler::AccumulatorMs(FP::Objects),
             FrameProfiler::AccumulatorMs(FP::Characters),
             FrameProfiler::AccumulatorMs(FP::Items),
             FrameProfiler::AccumulatorMs(FP::Effects),
             FrameProfiler::AccumulatorMs(FP::UI));
```

- [ ] **Step 4: Build and visually verify**

Run: `cmake --build --preset windows-x86-debug`
Expected: clean build (a wrong-length `kPassNames` would fail here).
Then launch and read the overlay: `U:` present, ~0 closed, >0 with windows open.

- [ ] **Step 5: Commit**

```bash
git add src/source/Core/Utilities/FrameProfiler.h src/source/Scenes/MainScene.cpp src/source/Scenes/SceneManager.cpp
git commit -m "perf(ui): add UI frame-profiler pass + overlay field"
```

---

### Task 1: UiStats counters + ghost-update trace + overlay line

**Goal:** The overlay reports, per frame: total NewUI windows, how many updated, how many rendered, how many updated **while hidden** (ghosts), and text draw/raster counts — and a debug build logs the class name of each ghost-updating window (the whitelist source for Task 2).

**Files:**
- Create: `src/source/UI/NewUI/UiStats.h`
- Modify: `src/source/UI/NewUI/NewUIManager.cpp:182-216` (Update/Render)
- Modify: `src/source/UI/Legacy/UIControls.cpp` (`CUIRenderTextOriginal::RenderText` ≈2624, `UploadText` ≈2751)
- Modify: `src/source/Scenes/SceneManager.cpp:578` (print + reset)

**Acceptance Criteria:**
- [ ] New header `UiStats.h` exposes `UiStats::Frame()` counters + `UiStats::ResetFrame()`.
- [ ] `windowsTotal/windowsUpdated/windowsRendered/ghostUpdates` are populated by `CNewUIManager::Update`/`Render`.
- [ ] `textDraws` increments per `RenderText` call; `textRasterized` increments per `UploadText` (GPU upload).
- [ ] Overlay shows a `UI win:.. upd:.. ren:.. ghost:.. text:.. raster:..` line; counters reset each frame after print.
- [ ] In a `_DEBUG`/`UI_GHOST_TRACE` build, opening then closing inventory prints `ui-ghost-update: <class>` lines for windows that update while hidden.

**Verify:** `cmake --build --preset windows-x86-debug`, launch with `MU_TEST_CHARS=50` + `MU_NOVSYNC`, open/close inventory+stats → overlay `ghost:` and `raster:` are non-zero; capture the `ui-ghost-update:` class list from the debug log for Task 2.

**Steps:**

- [ ] **Step 1: Create the counters header**

Create `src/source/UI/NewUI/UiStats.h`:

```cpp
#pragma once
// Per-frame UI cost counters for the $details overlay. Header-only, single-thread
// (render thread), mirrors FrameProfiler's pattern. Reset once per frame after the
// overlay reads the values. Intended for ad-hoc UI bottleneck hunting.

namespace UiStats
{
    struct Counters
    {
        int windowsTotal;     // size of m_vecUI this frame
        int windowsUpdated;   // Update() actually invoked
        int windowsRendered;  // Render() actually invoked
        int ghostUpdates;     // Update() invoked while !IsVisible()
        int textDraws;        // RenderText() calls
        int textRasterized;   // GDI->GPU uploads (UploadText)
    };

    inline Counters& Frame()
    {
        static Counters s_c{};
        return s_c;
    }

    inline void ResetFrame() { Frame() = Counters{}; }
}
```

- [ ] **Step 2: Instrument the NewUI update loop**

In `src/source/UI/NewUI/NewUIManager.cpp`, add includes near the top (after line 3):

```cpp
#include "UI/NewUI/UiStats.h"
#include <typeinfo>
```

Replace `CNewUIManager::Update()` (lines 182-199) with the instrumented version (logic unchanged — counters only; the gate is still `IsEnabled()`, Task 2 changes that):

```cpp
bool SEASON3B::CNewUIManager::Update()
{
    std::sort(m_vecUI.begin(), m_vecUI.end(), CompareLayerDepth);

    UiStats::Frame().windowsTotal = (int)m_vecUI.size();

    for (auto vi = m_vecUI.begin(); vi != m_vecUI.end(); vi++)
    {
        if ((*vi)->IsEnabled())
        {
            if (!(*vi)->IsVisible())
            {
                UiStats::Frame().ghostUpdates++;
#if defined(_DEBUG) || defined(UI_GHOST_TRACE)
                __TraceF(TEXT("ui-ghost-update: %hs\n"), typeid(**vi).name());
#endif
            }
            UiStats::Frame().windowsUpdated++;
            if (false == (*vi)->Update())
                return false;
        }
    }

    return true;
}
```

- [ ] **Step 3: Instrument the NewUI render loop**

Replace `CNewUIManager::Render()` (lines 201-216) — add the rendered counter only:

```cpp
bool SEASON3B::CNewUIManager::Render()
{
    std::sort(m_vecUI.begin(), m_vecUI.end(), CompareLayerDepth);
    auto vecUI = m_vecUI;

    auto vi = vecUI.begin();
    for (; vi != vecUI.end(); vi++)
    {
        if ((*vi)->IsVisible())
        {
            UiStats::Frame().windowsRendered++;
            (*vi)->Render();
        }
    }

    return true;
}
```

- [ ] **Step 4: Instrument the GDI text path**

In `src/source/UI/Legacy/UIControls.cpp`, add `#include "UI/NewUI/UiStats.h"` with the other includes. At the **first line of the body** of `CUIRenderTextOriginal::RenderText(...)` (≈2624) add:

```cpp
    UiStats::Frame().textDraws++;
```

At the **first line of the body** of `CUIRenderTextOriginal::UploadText(...)` (≈2751) add:

```cpp
    UiStats::Frame().textRasterized++;
```

- [ ] **Step 5: Print the UI stats line and reset**

In `src/source/Scenes/SceneManager.cpp`, add `#include "UI/NewUI/UiStats.h"` near the includes. Replace the existing reset (line 578) with a print-then-reset block:

```cpp
    g_pRenderText->RenderText((int)DEBUG_TEXT_X, y, szLine); y += DEBUG_TEXT_LINE_HEIGHT;

    {
        const UiStats::Counters& u = UiStats::Frame();
        swprintf(szLine, L"UI  win:%d upd:%d ren:%d ghost:%d  text:%d raster:%d",
                 u.windowsTotal, u.windowsUpdated, u.windowsRendered,
                 u.ghostUpdates, u.textDraws, u.textRasterized);
        g_pRenderText->RenderText((int)DEBUG_TEXT_X, y, szLine); y += DEBUG_TEXT_LINE_HEIGHT;
    }

    FrameProfiler::ResetFrame();
    UiStats::ResetFrame();
```

(Note: the `Frame ms` `swprintf` + its `RenderText` at lines 571-577 stay; this block goes immediately after, replacing the lone `FrameProfiler::ResetFrame();`.)

- [ ] **Step 6: Build, run, capture baseline + ghost list**

Run: `cmake --build --preset windows-x86-debug`
Then launch with the harness, open/close inventory+stats+character.
Expected: overlay `UI` line populates; `ghost:` > 0; debug log contains `ui-ghost-update: <class>` lines. **Save that class list** — it is the input to Task 2.

- [ ] **Step 7: Commit**

```bash
git add src/source/UI/NewUI/UiStats.h src/source/UI/NewUI/NewUIManager.cpp src/source/UI/Legacy/UIControls.cpp src/source/Scenes/SceneManager.cpp
git commit -m "perf(ui): add per-frame UI counters + ghost-update trace to overlay"
```

---

### Task 2: Suspend Update() for hidden windows (gated + reversible)

**Goal:** Hidden NewUI windows stop running `Update()` every frame, except a small whitelist that genuinely needs hidden ticks (discovered in Task 1) — controlled by a runtime kill-switch so the change is instantly reversible.

**Files:**
- Modify: `src/source/UI/NewUI/NewUIBase.h:24-51` (add `WantsHiddenUpdate()` virtual)
- Modify: `src/source/UI/NewUI/NewUIManager.h:16-27` (kill-switch extern)
- Modify: `src/source/UI/NewUI/NewUIManager.cpp:182-199` (gate + switch definition)
- Modify: the specific `NewUI*Window` classes flagged by Task 1's `ui-ghost-update` log (override `WantsHiddenUpdate`)

**Acceptance Criteria:**
- [ ] `CNewUIObj` has `virtual bool WantsHiddenUpdate() const { return false; }`.
- [ ] A global `bool g_bUiSuspendHiddenUpdate` (default `true`) gates the optimization; setting it `false` restores exact legacy behavior.
- [ ] `Update()` runs a window only when `IsEnabled() && (!g_bUiSuspendHiddenUpdate || IsVisible() || WantsHiddenUpdate())`.
- [ ] Every class printed by Task 1's ghost trace overrides `WantsHiddenUpdate()` to return `true`.
- [ ] With the switch on: overlay `ghost:` drops to only the whitelisted count; `U:` ms drops vs the Task 1 baseline; no functional regression (chat still receives while closed, event timers still fire, close/open animations intact).

**Verify:** `cmake --build --preset windows-x86-debug`, launch with `MU_TEST_CHARS=50` + `MU_NOVSYNC`. Compare overlay before/after: **baseline** = run with `g_bUiSuspendHiddenUpdate=false` (record `U:` and `ghost:`); **optimized** = `true` (record again). `U:` must be lower and `ghost:` near-zero, with chat/whisper/event windows still functioning when closed.

**Steps:**

- [ ] **Step 1: Add the opt-in virtual to the base class**

In `src/source/UI/NewUI/NewUIBase.h`, inside `CNewUIObj` (after `Enable`, before the `IsVisible` overrides, around line 45):

```cpp
        void Enable(bool bEnable)
        {
            m_bUpdate = bEnable;
        }

        // Windows that must keep ticking while hidden (network state accumulation,
        // close/open animation, timers) override this to return true. Default: a
        // hidden window does no per-frame work. See docs/ui/UI_MAP.md H2.
        virtual bool WantsHiddenUpdate() const { return false; }

        bool IsVisible() const override { return m_bRender; }
```

- [ ] **Step 2: Declare the kill-switch**

In `src/source/UI/NewUI/NewUIManager.h`, inside `namespace SEASON3B` above `class CNewUIManager` (after line 15):

```cpp
    // Master switch for the "don't Update hidden windows" optimization. true =
    // optimization on. Flip to false at runtime to restore exact legacy behavior
    // (every enabled window Updates regardless of visibility).
    extern bool g_bUiSuspendHiddenUpdate;
```

- [ ] **Step 3: Define the switch and apply the gate**

In `src/source/UI/NewUI/NewUIManager.cpp`, define the global after `using namespace SEASON3B;` (line 6):

```cpp
bool SEASON3B::g_bUiSuspendHiddenUpdate = true;
```

Then update the gate in `CNewUIManager::Update()` (the `if ((*vi)->IsEnabled())` block from Task 1):

```cpp
        CNewUIObj* pObj = *vi;
        if (pObj->IsEnabled())
        {
            const bool bHidden = !pObj->IsVisible();
            if (bHidden)
            {
                UiStats::Frame().ghostUpdates++;
#if defined(_DEBUG) || defined(UI_GHOST_TRACE)
                __TraceF(TEXT("ui-ghost-update: %hs\n"), typeid(*pObj).name());
#endif
            }

            const bool bRun = !g_bUiSuspendHiddenUpdate
                              || !bHidden
                              || pObj->WantsHiddenUpdate();
            if (bRun)
            {
                UiStats::Frame().windowsUpdated++;
                if (false == pObj->Update())
                    return false;
            }
        }
```

- [ ] **Step 4: Whitelist the windows that need hidden ticks**

For **each** class name captured from Task 1's `ui-ghost-update:` log, open that window's header and add the override. Example for a chat-type window (repeat per flagged class — do not guess the list, use the captured log):

```cpp
    // In e.g. src/source/UI/Chat/Chat.h, inside the window class:
    bool WantsHiddenUpdate() const override { return true; } // receives messages while closed
```

Rule of thumb for deciding: keep `true` if the window (a) consumes network packets, (b) runs a countdown/timer the player must not lose, or (c) plays a close animation. Otherwise leave default `false`.

- [ ] **Step 5: Build and A/B verify (switch off = baseline, on = optimized)**

Run: `cmake --build --preset windows-x86-debug`, launch with harness.
- Set `g_bUiSuspendHiddenUpdate = false` (debugger or temporary default), record overlay `U:` and `ghost:` = **baseline**.
- Set back to `true`, record again = **optimized**.
Expected: optimized `U:` < baseline `U:`; `ghost:` ≈ whitelist size; chat/whisper/event windows still work while closed.

- [ ] **Step 6: Commit**

```bash
git add src/source/UI/NewUI/NewUIBase.h src/source/UI/NewUI/NewUIManager.h src/source/UI/NewUI/NewUIManager.cpp
git add src/source/UI/   # + the whitelisted window headers touched in Step 4
git commit -m "perf(ui): suspend Update() for hidden windows (reversible kill-switch + whitelist)"
```

---

## Future Roadmap (design-level — expand into tasks when reached)

These phases are deliberately NOT broken into step-by-step tasks yet — doing so now would be speculation. Each entry states the **seam contract** and the **mobile payoff**. Expand the phase into a detailed task file (same format as above) at the moment you start it.

### Phase 2 — Render seam: `IUiBackend` / `RenderQueue`
- **Contract:** UI stops calling `RenderBitmap`/`RenderColor`/`glBegin` directly (`ZzzOpenglUtil.cpp:1198`). It enqueues `DrawCmd{ texture/atlas, srcUV, dstRect, color }`. One `IUiBackend::Flush()` translates the queue to GL. Migrate call sites mechanically (`RenderImage(...)` → `ctx.draw.Image(...)`).
- **Enables:** batch by texture (kills per-quad bind churn, `UI_MAP.md` H7), and a swappable backend.
- **Mobile payoff:** implement a second `IUiBackend` over **GLES/Metal/Vulkan** with zero changes to window code.
- **Risk:** medium — broad but mechanical call-site migration; keep the GL backend bit-identical first (no batching), then add batching behind it.

### Phase 3 — Text seam: `ITextRenderer` (glyph atlas)
- **Owned by `docs/perf/06-ui-text-cache.md`.** Do not duplicate; unify under this seam.
- **Contract:** replace the per-string GDI→`glTexSubImage2D` path (`UIControls.cpp:2624+`) with a glyph atlas + string→quad cache keyed by (text, font, color), invalidated on change.
- **Enables:** the biggest CPU win (H1, the ~5.5 ms/frame text cost).
- **Mobile payoff:** **GDI/`HFONT`/`CreateDIBSection` do not exist on Android/iOS** — this seam is mandatory for any port. Back it with FreeType/stb_truetype.
- **Risk:** medium — correctness of cache invalidation; measured directly by Task 1's `text/raster` counters.

### Phase 4 — Input/context seam: `IUiInput` + `FrameContext`
- **Contract:** widgets stop reading globals (`MouseX`, `GetFocus()`, `WorldTime`, `WindowWidth`). They receive `FrameContext{ dt, input, screenW, screenH, dpiScale }` by parameter; an `InputRouter` does z-ordered hit-test → focus capture → dispatch.
- **Enables:** fewer focus bugs, testable widgets.
- **Mobile payoff:** add **touch** as an `InputEvent` source; DPI/scale via `FrameContext`; "fat-finger" hit areas in the router.
- **Risk:** medium — click/focus regressions; migrate one input source at a time.

### Phase 5 — Unify 3 managers → 1 facade `UiManager`
- **Contract:** adapt `CUIMng` (CWin) and `CUIWindowMgr` behind the same facade as `CNewUIManager`; retained `UiTree` with `Dirty{Layout,Visual,Text}` flags.
- **Enables:** one update/render/input path; dirty-flag invalidation instead of recompute-all.
- **Mobile payoff:** indirect (single surface to port).
- **Risk:** med-high — touches login/char-select (CWin) and old inventory (CUIWindowMgr).

### Phase 6 — Mobile bring-up
- **Contract:** implement the GLES `IUiBackend`, FreeType `ITextRenderer`, touch `IUiInput`; wire DPI/safe-area into `FrameContext`; portable asset paths.
- **Enables:** runs on Android/iOS.
- **Mobile payoff:** the port itself — now mechanical because Phases 2–4 removed every desktop-only dependency.
- **Risk:** medium — platform plumbing, but no UI-logic rewrite.

### Phase 7 — Optional: new screen via RmlUi (behind `IUiBackend`)
- **Contract:** only for a genuinely-new, layout-heavy screen (e.g. web-like cash shop). Implement RmlUi's render/system interfaces on top of `RenderQueue`. **Never** migrate existing inventory/stats/skill screens.
- **Mobile payoff:** data-driven HTML/CSS, good mobile story.
- **Risk:** high — new dependency; gated by "is the screen new AND layout-heavy AND seams already exist".

---

## "Do now / avoid now" guardrails (apply from Task 0 onward)

**Do now:**
- [ ] New UI code takes a `FrameContext` param; never reads `WorldTime`/`MouseX`/`WindowWidth` directly.
- [ ] No new `glBegin`/`HFONT`/`HDC`/`CreateDIBSection` in UI code — go through a helper even if the seam isn't built yet (redirect later).
- [ ] Sizes in logical units × `dpiScale`, not hardcoded pixels.
- [ ] Portable asset paths (no `\` separators).

**Avoid now:**
- [ ] Replacing the framework wholesale.
- [ ] Touching the per-frame sorts/copies (micro-cost at current N — see "Deferred").
- [ ] Migrating existing working screens to any external UI lib.

---

## Self-review checklist (run before executing)

- [ ] Task 0 array length: `kPassNames` has exactly `Count_` entries after adding `UI`.
- [ ] Task 1 `UiStats.h` symbols (`Frame()`, `ResetFrame()`, `Counters`) match every use in NewUIManager/UIControls/SceneManager.
- [ ] Task 2 gate expression matches the counter names introduced in Task 1.
- [ ] `g_bUiSuspendHiddenUpdate` declared (`.h` extern) and defined (`.cpp`) exactly once.
- [ ] No task assumes a window-class whitelist hardcoded at plan time — it is data from Task 1's runtime log.
