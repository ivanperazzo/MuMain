# UI System Map & Optimization Plan — MuMain Client

> Architecture audit of the MuMain client UI/rendering, grounded in source (`file:line`).
> Where a claim could not be confirmed from source it is marked **ASSUMPTION**.
> Scope: identify wasted CPU/GPU in the UI, lay out an incremental optimization
> plan, and decide refine-vs-replace + a cross-platform path.
>
> Date: 2026-06-14 · Author: engine/UI audit

---

## 0. Structural finding that conditions everything

MuMain has **three UI systems coexisting**, which explains much of the problem surface.

| System | Manager | Container | Scenes | Visibility gate |
|---|---|---|---|---|
| **Legacy CWin** | `CUIMng` (`UI/Legacy/UIMng.cpp`) | `CPList m_WinList` | login / char-select / char-make | Render has **no manager-level gate** (`UIMng.cpp:761-775`) — relies on internal early-out **(ASSUMPTION)** |
| **Legacy CUIWindowMgr** | `CUIWindowMgr` (`UI/Legacy/UIWindows.cpp`) | `std::map m_WindowMap` + `std::list m_WindowArrangeList` (z-order) | old inventory/trade/storage | Yes: `state != UISTATE_HIDE/READY` (`UIWindows.cpp:306,321`) |
| **NewUI** (target system) | `SEASON3B::CNewUIManager` (`UI/NewUI/NewUIManager.cpp`) | `std::vector m_vecUI` + `std::map m_mapUI` | everything newer | Render gated by `IsVisible()`; **Update gated by `IsEnabled()`, NOT visibility** |

NewUI base: `INewUIBase` / `CNewUIObj` in `NewUIBase.h`.

Per-frame entry point:
`Winmain.cpp:1100` → `RenderScene` (`SceneManager.cpp:1044`) → `MainScene` →
`RenderMainSceneUI` (`MainScene.cpp:530`) → `RenderInterface()` (legacy) +
`g_pNewUISystem->Render()` (NewUI).

---

## 1. Community signals → hypotheses (NOT ground truth)

Forums not browsed in this audit. Uses recognized MU-community patterns + internal
note `perf-forum-ref` (tuservermu topic 87711: devs optimizing *this* client —
terrain 2 passes, VBO/culling/GPU skinning). Each signal converted to an
inspectable hypothesis here:

| Known community signal | Hypothesis about MuMain | Where to look |
|---|---|---|
| "Old/limited fixed-function engine" | Immediate-mode 2D `glBegin` per quad, no VBO | `ZzzOpenglUtil.cpp:1198` ✅ confirmed |
| "Doesn't use modern hardware / CPU-bound" | Matches `perf-release-skinning` note: GPU idle, CPU is the wall | confirmed outside UI |
| "Lag with multiple instances" | Each instance re-renders text via GDI + `glTexSubImage2D` per string per frame; nothing shareable across processes | `UIControls.cpp:2624+` ✅ |
| "Driver/GPU-dependent graphics bugs" | Uses `GL_ALPHA_TEST` + 24-bit DIB + `gluPerspective`/`gluOrtho2D` (legacy GLU) → driver-variable behavior | `ZzzOpenglUtil.cpp:316,1084` |
| "Remakes to DX11/modern" | Temptation to swap renderer; **trap**: UI cost is CPU (text + hidden update + sorting), not the backend | see §2 |
| "UI hard to customize" | 3 systems, pixel-hardcoded layout, not data-driven | `INTERFACE_*` enums `UIManager.h:18-53` |

**Conclusion §1:** signals point at CPU/architecture, not "missing DX11". Swapping
the renderer does NOT fix UI cost.

---

## 2. Expected technical diagnosis (and what actually applies)

| Typical legacy-MMORPG failure | Present in MuMain? | Impact | Manifestation |
|---|---|---|---|
| Hidden windows still **update** | **YES** — `Update` gated by `IsEnabled()`=`m_bUpdate`, not `m_bRender` (`NewUIManager.cpp:189`) | CPU linear in #created windows | flat cost even with everything closed |
| Hidden windows still **draw** | NewUI **NO** (gated by `IsVisible`); CWin **likely YES** (ASSUMPTION) | GPU+CPU ghost draw | overdraw at login |
| Global per-frame widget update | **YES** — iterates full `m_vecUI` always | CPU | scales with total windows |
| No dirty flags | **YES** — no invalidation; everything recomputed | CPU | text/layout every frame |
| Layout recomputed always | **NO** (positions cached `m_iPos_x/y`) | low | — |
| **Costly text rendering** | **YES, the worst** — GDI→memcpy→`glTexSubImage2D`→quad per string per frame, no glyph cache | high CPU + PCIe upload | the ~5.5ms/frame from internal note |
| Global input routing | **PARTIAL** — NewUI routes by z-order, but CWin/controls poll global `MouseX/MouseY` + `g_dwMouseUseUIID` | low CPU, focus bugs | "ghost" clicks |
| No atlas/material batching | **YES** — no atlas, `BindTexture` per quad (cached but per-quad) | draw calls + binds | many UI draw calls |
| HUD / modal / overlay mixed | **YES** — all in `RenderMainSceneUI`, no separate passes | maintainability | hard to isolate cost |
| UI logic coupled to render loop | **YES** — formatting (`mu_swprintf`) and animation (`sinf(WorldTime)`) inside `Render()` | CPU + not testable | new string per frame |

**Extra hidden cost (own finding, not in the standard list):** `CNewUIManager` does,
**per frame, regardless of visibility**:

- **4× `std::sort`** of the full vector: `Update`(L184), `Render`(L203), `UpdateMouseEvent`(L111), `UpdateKeyEvent`(L147).
- **3× full vector copy**: `auto vecUI = m_vecUI` (L112, L149, L204).
- **O(N²)** in mouse: linear `std::find` inside the loop (L123).

With N created (not visible) windows, you pay `4·N·logN` comparisons + `3·N` pointer
copies each frame even on an empty screen.

```cpp
// UI/NewUI/NewUIManager.cpp:182  (representative — Render/Mouse/Key are analogous)
bool SEASON3B::CNewUIManager::Update()
{
    std::sort(m_vecUI.begin(), m_vecUI.end(), CompareLayerDepth);   // every frame
    for (auto vi = m_vecUI.begin(); vi != m_vecUI.end(); vi++)
        if ((*vi)->IsEnabled())            // <-- NOT IsVisible(): hidden windows still Update
            if (false == (*vi)->Update())
                return false;
    return true;
}
```

---

## 3. What to inspect in MuMain (confirmed map)

| Concept | Confirmed location |
|---|---|
| Window manager (target) | `CNewUIManager` — `UI/NewUI/NewUIManager.cpp` |
| Legacy window managers | `CUIMng` `UI/Legacy/UIMng.cpp`; `CUIWindowMgr` `UI/Legacy/UIWindows.cpp` |
| Window base / tree | `INewUIBase`/`CNewUIObj` `NewUIBase.h`; legacy `CWin` `UI/Widgets/Win.h`, `CUIControl` `UIControls.h` |
| Window list | `m_vecUI`/`m_mapUI` (NewUI); `m_WinList` (CWin); `m_WindowMap`/`m_WindowArrangeList` (CUIWindowMgr) |
| UI update loop | `CNewUIManager::Update` `NewUIManager.cpp:182`; `CUIMng::Update` `UIMng.cpp:626` |
| UI render loop | `CNewUIManager::Render` `NewUIManager.cpp:201`; `CUIMng::Render` `UIMng.cpp:761`; orchestration `RenderMainSceneUI` `MainScene.cpp:530` |
| Visibility flags | `m_bRender` / `m_bUpdate` (`NewUIBase.h:27`, `IsVisible()/IsEnabled()` L47-48); `m_bShow/m_bActive/m_nState` (`Win.h:28-31`); `UISTATE_HIDE/READY` (`UIControls.h:31-35`) |
| Focus/active/enabled | `g_dwActiveUIID`, `g_dwTopWindow`, `g_dwMouseUseUIID` (`UIControls.cpp`); `m_pActiveMouseUIObj`/`m_pActiveKeyUIObj` (NewUI) |
| Z-order | `GetLayerDepth()` + `std::sort(CompareLayerDepth)` (NewUI); `m_WindowArrangeList` (CUIWindowMgr) |
| Input dispatch | `UpdateMouseEvent`/`UpdateKeyEvent` `NewUIManager.cpp:107,144`; `CheckMouseIn` `UIControls.cpp:145` |
| Text rendering | `CUIRenderTextOriginal::RenderText` `UIControls.cpp:2624+` → `WriteText`(2703) → `UploadText`(2751); via `g_pRenderText` |
| UI resource loading | `LoadBitmap(...)` in each window's `Create()` (e.g. `NewUICharacterInfoWindow.cpp:1566`) — cached ✅ |
| Atlas/textures | **No atlas**; `Bitmaps[tex].TextureNumber`, `BindTexture` `ZzzOpenglUtil.cpp:157` |
| HUD/gameplay hooks | `RenderMainSceneUI` `MainScene.cpp:530` (mixes HUD+windows+cursor); `g_pPartyManager->Render` |

**To validate (ASSUMPTION):** that each legacy `CWin::Render()` early-outs on
`m_bShow`. Validation: counter in `RenderBitmap`, open/close login, check the count
drops when hidden.

---

## 4. Prioritized hypotheses

| # | Hypothesis | Impact | Verify ease | Fix |
|---|---|---|---|---|
| H1 | **Text re-rasterized/uploaded every frame** (GDI→`glTexSubImage2D` per string) dominates UI CPU | **High** | Easy (count `RenderText` + timer around `UploadText`) | Glyph atlas; string→texture cache with dirty flag |
| H2 | **Hidden windows still in `Update()`** (`IsEnabled` ≠ `IsVisible`) | Med-High | Easy (log `Update()` for windows with `m_bRender==false`) | Gate `Update` by visibility too, or `Enable(false)` on `Show(false)` |
| H3 | **4 sorts + 3 copies/frame** in NewUIManager | Med (scales with N) | Easy (timer in the 4 methods) | Keep incremental order (sort on insert/z-change only); iterate without copy |
| H4 | **O(N²)** in `UpdateMouseEvent` (`std::find` in loop) | Low-Med | Medium | Stable index/iterator; avoid `find` |
| H5 | `mu_swprintf` per frame in the Render path | Med | Easy (count formats) | Format only on-change (dirty) |
| H6 | UI animations (`sinf(WorldTime)`) computed even when irrelevant/unfocused | Low | Easy | Skip if hidden / off-screen |
| H7 | Excess UI draw calls from missing atlas/batch | Med (GPU/driver) | Medium (count `glBegin`/binds) | Atlas + batch by texture |
| H8 | CWin legacy draws hidden (no manager gate) | Med | Easy | Explicit gate in `CUIMng::Render` |
| H9 | GL state misuse (legacy `GL_ALPHA_TEST`, toggles) causes driver-dependent bugs | Low-Med | Hard (GPU-dependent) | Migrate alpha-test to discard/standard blend |

**Attack order by ROI:** H1 → H2 → H3 → H5 → H7 → rest.

---

## 5. Code-audit plan (concrete instrumentation)

Measurable base already exists: `CalcFPS` (`ZzzAI.cpp:711`), `FrameProfiler` with
passes **T/O/C/I/E** (`SceneManager.cpp:568`), frame graph and 1%-low
(`SceneManager.cpp:95-174`), `FrameTimerScheduler`. **Missing: a `U` (UI) pass.**

**Step 1 — add a UI pass to the existing profiler:**
```cpp
// FrameProfiler::Pass { Terrain, Objects, Characters, Items, Effects, UI }
FrameProfiler::Begin(Pass::UI);
  RenderInterface(true);
  g_pNewUISystem->Render();
FrameProfiler::End(Pass::UI);   // overlay already prints "U: x.xx ms"
```

**Step 2 — per-frame counters (static struct, dumped to overlay):**
```cpp
struct UiStats {
  int windowsTotal, windowsUpdated, windowsRendered;   // H2,H3
  int textDrawCalls, textRasterized, textCacheHits;    // H1,H5
  int quadDrawCalls, textureBinds;                      // H7
  double msUpdate, msRender, msText, msUpload, msSort;  // CPU split
};
```
- `windowsUpdated++` inside the `if(IsEnabled())`, log if `!IsVisible()` → measures H2 directly.
- `textRasterized++` in `WriteText`, `textCacheHits++` when (future) cache hits → measures H1.
- `quadDrawCalls++` in `RenderBitmap`, `textureBinds++` only when `CachTexture != tex` → measures H7.
- `msSort` with `QueryPerformanceCounter` around the 4 `std::sort` → measures H3.

**Step 3 — split CPU-UI vs GPU-UI:** instrument CPU with QPC; for GPU use
`glBeginQuery(GL_TIME_ELAPSED)` around `RenderMainSceneUI` only. If CPU≫GPU
(hypothesis), confirms the backend is NOT the problem.

**Step 4 — detect hidden-but-consuming:** temporary log in each `CNewUIObj::Update()`:
`if(!IsVisible() && IsEnabled()) Log("ghost-update %s", name)`.

**Baseline to capture:** `msText`, `msUpdate`, `windowsUpdated` for (a) empty
in-game screen, (b) inventory+stats open, (c) login. Diff (a) vs everything-closed
reveals the flat cost of H2/H3.

---

## 6. Incremental improvements (layered, no rewrite)

| # | Improvement | Expected benefit | Risk | Complexity | Prereq |
|---|---|---|---|---|---|
| M1 | **Gate `Update` by visibility** in `CNewUIManager::Update` (or `Enable(false)` on `Show(false)`) | Cuts flat cost of hidden windows (H2) | Low — some windows may need hidden ticks (whisper, event timers) → audit which | Low | List of windows that MUST update while hidden |
| M2 | **Text cache string→texture with dirty flag** | Removes GDI+upload per frame (H1, highest ROI) | Med — correct invalidation | Med | Hash(text+font+color) → texture handle |
| M3 | **Incremental sort**: order only on insert/z-change; iterate `m_vecUI` without copy | Removes 4 sorts + 3 copies/frame (H3) | Low | Low | `m_bDirtyOrder` flag |
| M4 | **Format dirty flag**: `mu_swprintf` only on-change | Removes per-frame formatting (H5) | Low | Low | M2 helps |
| M5 | **Explicit render gate in CWin** (`CUIMng::Render`) | Cuts hidden legacy draw (H8) | Low | Low | Confirm current early-out |
| M6 | **UI atlas + batch by texture** | Fewer binds/draw calls (H7) | Med — pack assets | Med-High | Offline atlas packer |
| M7 | **Separate passes**: HUD / Windows / Debug-overlay with `FrameProfiler` | Measurable + maintainable | Low | Low | Step 1 §5 |
| M8 | **Focus-based input router** (consolidate `g_dwMouseUseUIID`/polling) | Fewer focus bugs, less polling | Med — click regressions | Med | Hit-test map |
| M9 | **Lazy init** of rare windows (events) | Less memory/texture resident | Low | Low | Create on-first-open |
| M10 | **Real suspension** of background windows (unload textures) | Less VRAM with many instances | Med | Med | M1 + texture refcount |
| M11 | **Fix O(N²) mouse** (manager's M4) | Cleanup, scaling | Low | Low | — |

**Recommended sequence:** M7 (measure) → M3+M11 (free, no risk) → M1 (high ROI, bounded
risk) → M2+M4 (highest ROI) → M5 → M6 → M8/M9/M10.

---

## 7. Target UI architecture

Decouple into **explicit passes** behind a facade, without throwing away NewUI:

```
UiManager (single facade; absorbs CNewUIManager + adapts CWin/CUIWindowMgr)
 ├─ UiTree            // retained hierarchy; nodes with parent/children
 ├─ per-node state:   { Visibility(Render), Active(Update/Input), Dirty(Layout|Visual|Text) }
 ├─ LayoutPass        // only DirtyLayout nodes (anchors/DPI) — already cached today
 ├─ UpdatePass        // only Active && Visible (except "hidden-tick" whitelist)
 ├─ RenderPass        // only Visible; emits to RenderQueue (no direct GL)
 │    └─ RenderQueue → batch by atlas/material → single flush
 ├─ TextSubsystem     // glyph atlas + string→quad cache; no GDI/frame
 ├─ InputRouter       // hit-test by z-order → capture focus → route to node (no global polling)
 ├─ ModalStack        // overlays/dialogs; blocks input below
 └─ HudPass           // separate: bars/buffs/chat, no modals
```

**Decoupling rules:**
- UI does **not** call `glBegin` directly → enqueues `DrawCmd{atlas, uv, rect, color}`;
  one backend translates to GL (today) or another backend (tomorrow). This is the
  **backend abstraction** that enables §10.
- UI does **not** read `WorldTime`/`MouseX` globals → receives `FrameContext{dt, input}`
  by parameter. Testable and portable.
- Gameplay→UI via events/observed state, not UI reading gameplay structs in `Render`.

---

## 8. Strategic decision: refine vs replace

**Recommendation: refine + encapsulate behind a facade. Do NOT replace now.** Migrate
*new* screens onto the already-unified NewUI base.

| Criterion | Refine own UI (recommended) | Full replacement |
|---|---|---|
| Integration cost | Low — wins (M1-M5) are days, not months | Very high — rewrite dozens of `NewUI*Window` |
| Regression risk | Bounded per improvement | Very high — MU look&feel is bespoke, asset-driven |
| Mobile future | Enabled by §7/§10 without replacing | You must abstract the backend either way |
| Tooling | ImGui already available for debug | — |
| Performance | The bottleneck (text/hidden-update) is fixed without changing framework | No guaranteed gain: cost is own CPU, not the framework |
| Maintainability | Improves by unifying 3→1 + passes | Better long-term, worse short-term |
| MU look&feel | Identical (same asset pipeline) | Risk of "doesn't feel like MU" |

**Decision cut:** measure first (§5). If after M1-M6 UI CPU drops to target and GPU
stays idle → don't replace. Only consider partial replacement if a large new screen
(e.g. web-like cash shop) justifies HTML/CSS.

---

## 9. Future alternatives

| Option | Correct role | Integration with own engine | Desktop/Mobile | Cost/License |
|---|---|---|---|---|
| **Refined own UI** | **Player runtime** | Native (already integrated) | Yes, via §10 | Free |
| **Dear ImGui** | **Tooling/debug/editor** (not runtime) | Trivial (imgui submodule already, `_EDITOR`) | Desktop; weak mobile | MIT |
| **RmlUi** | Runtime candidate *for new screens* HTML/CSS | Medium — implement `RenderInterface`/`SystemInterface` (fits §7 RenderQueue) | Good desktop+mobile | MIT |
| **NoesisGUI** | Game-UI runtime (XAML/WPF-like) | Med-High — XAML pipeline, own runtime | Mature desktop+mobile | **Commercial** (license) |

**Key distinction:** ImGui = tooling/debug, not player UI (immediate-mode, dev
aesthetic). NoesisGUI = full C++ game-UI framework but **paid** and with its own
pipeline. RmlUi = lightweight C++ HTML/CSS lib, good middle ground if you want
data-driven + mobile.

**Recommendation:** ImGui **now** for tooling/editor; own UI for runtime; keep RmlUi
on radar for new screens *if* the §7 facade exists (then adding an Rml backend is
incremental). NoesisGUI only if the business justifies the license.

---

## 10. Mobile / cross-platform

Good base: already uses **SDL3** (portable input/window) and `FrameTimerScheduler`
(replaced Win32 `SetTimer`). The mobile ballast is: fixed-function render
(`gluPerspective`, `GL_ALPHA_TEST`, immediate mode) and **text via GDI/DIB
(`CreateDIBSection`, `HFONT`) — pure Win32, not portable**.

**DO NOW:**
- [ ] Isolate UI from direct GL behind `RenderQueue`/`DrawCmd` (§7) — enables GLES/Metal/Vulkan later.
- [ ] **Move text off GDI**: glyph atlas (FreeType/stb_truetype) — GDI doesn't exist on Android/iOS.
- [ ] Coordinates and sizes in logical units + DPI/scale factor (not hardcoded pixels as today).
- [ ] Abstract input `InputEvent` (already close with SDL) supporting **touch** beyond mouse.
- [ ] Hit-test tolerant of "fat fingers" (touch areas ≥ minimum) in `InputRouter`.
- [ ] Text/IME via SDL `SDL_StartTextInput` (not Win32 `GetKeyboardState`).

**AVOID NOW:**
- [ ] More new `gl` immediate-mode calls (don't scale to GLES).
- [ ] New dependencies on `HDC`/`HFONT`/`HWND`/`CreateDIBSection` in UI code.
- [ ] Assuming fixed resolution or 4:3 aspect / nonexistent safe-areas.
- [ ] Reading global desktop state (`MouseX`, `GetFocus()`) from widget logic.
- [ ] Packaging assets with Windows paths/separators.

---

## 11. Concrete deliverables (creatable in repo)

| # | Deliverable | Form | Effort |
|---|---|---|---|
| E1 | **UI system map** | this file (`docs/ui/UI_MAP.md`) | XS ✅ |
| E2 | **UI profiler** | `U` pass in `FrameProfiler` + `UiStats` (§5) | S |
| E3 | **UI metrics overlay** | extend `RenderDebugInfo` with `U:` line + counters | S |
| E4 | **Invalidation system** | `DirtyFlags{Layout,Visual,Text}` on `CNewUIObj` | M |
| E5 | **Real visibility (suspend hidden update)** | M1: gate Update + hidden-tick whitelist | S |
| E6 | **UI pass separation** | HUD / Windows / Overlay with timers | S |
| E7 | **Text cache** | `TextCache` string→texture (glyph atlas) | M-L |
| E8 | **Sort/iter without copy** | `m_bDirtyOrder` + in-place iterate | XS |
| E9 | **Focus-based input router** | consolidate global polling into `InputRouter` | M |
| E10 | **UI backend abstraction** | `RenderQueue`/`DrawCmd` + GL backend | L |
| E11 | **Refine-vs-replace ADR** | `docs/adr/000X-ui-refine-not-replace.md` | XS |
| E12 | **Mobile-readiness checklist** | `docs/ui/MOBILE_READINESS.md` (§10 list) | XS |

---

## Executive summary

1. UI cost is **own CPU** (per-frame GDI text + updating hidden windows + 4 sorts/3
   copies per frame), **not the renderer** — switching to DX11 won't fix it.
2. Cheap, zero-risk wins first: **measure (U pass) → incremental sort → suspend hidden
   update → text cache**.
3. **Refine and encapsulate**, don't replace; the `RenderQueue` facade leaves the door
   open to RmlUi and mobile without committing today.

---

### Appendix A — key `file:line` references (verified)

- `UI/NewUI/NewUIManager.cpp:107` `UpdateMouseEvent` — sort + copy + O(N²) `std::find`
- `UI/NewUI/NewUIManager.cpp:144` `UpdateKeyEvent` — sort + copy
- `UI/NewUI/NewUIManager.cpp:182` `Update` — sort; gated by `IsEnabled()` (not visibility)
- `UI/NewUI/NewUIManager.cpp:201` `Render` — sort + copy; gated by `IsVisible()`
- `UI/NewUI/NewUIBase.h:27,47-48` `m_bRender`/`m_bUpdate`, `IsVisible()/IsEnabled()`
- `UI/Legacy/UIMng.cpp:761-775` `CUIMng::Render` — no manager-level visibility gate
- `UI/Legacy/UIWindows.cpp:306,321` `CUIWindowMgr` Render/DoAction — state gate present
- `UI/Legacy/UIControls.cpp:2624+` `CUIRenderTextOriginal::RenderText` → `WriteText`(2703) → `UploadText`(2751) — GDI→memcpy→`glTexSubImage2D` per string/frame
- `Render/Textures/ZzzOpenglUtil.cpp:157` `BindTexture` — cached via static `CachTexture`, per-quad
- `Render/Textures/ZzzOpenglUtil.cpp:1198` `RenderBitmap` — `glBegin(GL_TRIANGLE_FAN)` per quad, no atlas
- `Render/Textures/ZzzOpenglUtil.cpp:1084` `BeginBitmap` — `gluPerspective`/`gluOrtho2D`, `GL_ALPHA_TEST`
- `Scenes/MainScene.cpp:530` `RenderMainSceneUI` — legacy `RenderInterface()` + `g_pNewUISystem->Render()`
- `Scenes/SceneManager.cpp:568` `FrameProfiler` passes T/O/C/I/E (no UI pass yet)
- `App/Platform/Windows/Winmain.cpp:1100` main loop
