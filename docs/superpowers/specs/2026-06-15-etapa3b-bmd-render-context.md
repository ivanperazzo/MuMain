# Etapa 3b — BMD render-context refactor (move per-render state off the shared model)

> Design + decomposition for the "big core-render refactor" the user chose (15-jun) to unblock
> Task 6 of the job system (`docs/superpowers/plans/2026-06-15-etapa3-job-system.md`). Builds on
> the audit `docs/perf/08-jobs-audit.md` and the BMD state map (Explore, 15-jun). Prereqs:
> Etapa 3 Tasks 1-5 landed (ThreadPool, WorkerArena, per-entity bone, per-worker buckets+merge,
> G/B/F restructure) — all serial-identical.

## Problem (why Task 6 was blocked)

`Models[]` is ONE shared `BMD` per model type (`ZzzOpenData.cpp:100-102`; `BMD* b = &Models[o->Type]`
everywhere; BMD copy-ctor is private). The per-entity render build writes **per-render scalar
state onto this shared object** (`this->BodyScale`, `BodyLight`, `CurrentAnimation`, …) during
`Animation`/`Transform`/`RenderMesh`, uses it, then the next entity overwrites it. Two entities
of the SAME type built in parallel race on every such member — and a crowd is mostly one type, so
"partition by type" gives ~0 parallelism in the case that matters. To parallelize Phase B we must
make the shared BMD **immutable during render** by moving the per-render state into a per-call /
per-worker context.

## Goal & invariant

Move all per-render mutable state off the shared `BMD` into a **`BmdRenderContext`** threaded
through the render call chain (and the coupled file-globals/statics into per-worker homes). After
this, building an entity reads immutable model data from the shared `BMD` and reads/writes only
its own context → safe to run concurrently for entities of the same type.

**Hard invariant every sub-task keeps:** serial-identical. `MU_JOBS` stays OFF until the final
switch; every intermediate commit produces pixel-identical output and identical `[bmd_cov]`
(inst=2300 @100ch). Verified by the A/B harness + in-game, exactly like Tasks 1-5.
**Do NOT change `sizeof(BMD)` in a way that shifts the `Models` array layout** (crash-login). Moving
a member OUT of BMD shrinks it — acceptable (the array is sized by `sizeof(BMD)` at alloc, no
external offset assumption beyond the random-offset guard), but verify the login scene after the
member-removal steps.

## The context object

```cpp
// Render/Build/BmdRenderContext.h  (per-entity, lives in the Task-5 VisibleChar / a per-worker slot)
struct BmdRenderContext
{
    // placement
    float  bodyScale = 1.f;
    vec3_t bodyOrigin{};
    float  bodyHeight = 0.f;
    float  boneScale = 1.f;          // the FILE-GLOBAL BoneScale (not the rarely-used BMD member)
    // lighting
    vec3_t bodyLight{};
    bool   lightEnable = true;
    bool   contrastEnable = false;
    vec3_t shadowAngle{};            // written mid-pipeline (Transform/lit-build)
    // animation
    vec3_t bodyAngle{};
    unsigned short currentAction = 0, priorAction = 0;
    float  currentAnimation = 0.f;
    short  currentAnimationFrame = 0;
    // mesh selection
    char   streamMesh = -1, skin = 0;
    bool   hideSkin = false;
    // outputs
    float  fTransformedSize = 0.f;
    // per-Transform correlation (the s_last*/serial statics) — see sub-task 6.6
    // flat color precomputed for the instanced flat branch (replaces glGetFloatv) — sub-task 6.7
    float  flatColor[4] = {1,1,1,1};
};
```
The render methods take `BmdRenderContext& ctx` (mutable: a few fields are written mid-pipeline)
instead of reading `this->`. The shared `BMD` keeps only immutable model data (group (a) in the map).

## Surface (from the BMD state map)

~16 per-render members; **BodyLight is the hot one (~40 reads)**, BodyOrigin ~18, BodyScale ~16,
LightEnable ~12, anim fields ~3-9 each, StreamMesh/HideSkin ~6-10. ~20 methods change signature
(`Animation`, `Transform`, `SkinMesh`, `Chrome`, `RenderMesh`, all `RenderBody*`/`RenderMesh*`
variants, `TransformPosition`/`RotationPosition`/`TransformBy*`, `ComputeInstLitLight`, shadow/coin
helpers). Setup is scattered: ~80 `b->member = o->…` sites across `ZzzObject.cpp`,
`ZzzCharacter.cpp`, `ZzzEffect.cpp`. Coupled non-member state: file-global `BoneScale`, the
per-Transform statics (`s_lastBoneMatrix`/`s_transformSerial`/`s_meshSkinned[]`/…), the `InstSet*`
collect globals, and the `glGetFloatv(GL_CURRENT_COLOR)` build-path GL read.

## Decomposition — serial-identical sub-tasks (each its own commit + A/B)

Each step moves one cohesive group from `BMD` member → `BmdRenderContext`, updating ALL write/read
sites in lockstep (a field can't be half-migrated — 40 read sites must move together). After each
step: build Release, `run-harness-ab.bat 1 1 <tag>` → `[bmd_cov]` identical, in-game spot-check.

- **6.1 — Plumb `BmdRenderContext` (no field moved yet).** Add the header + a per-worker context
  instance (and store one in the Task-5 `VisibleChar`/Phase-G entry). Thread `BmdRenderContext& ctx`
  as a parameter through the render call chain (`Animation`/`Transform`/`RenderBody*`/`RenderMesh*`/
  `SkinMesh`/`Chrome`/`TransformPosition`/`RotationPosition`/`ComputeInstLitLight`/shadow helpers).
  The param is UNUSED this step → trivially serial-identical. Establishes the plumbing so later steps
  only flip read/write sites. (Setup sites populate `ctx` from the OBJECT in Phase G, mirroring
  today's `b->member = o->…`, but the renderers still read `this->` until each field is migrated.)
- **6.2 — Placement group:** `BodyScale`, `BodyOrigin`, `BodyHeight`, file-global `BoneScale` →
  `ctx`. Update setup (Phase G) + ~50 read sites (SkinMesh, Transform, TransformPosition, lit-build,
  inst gate `:1553-1581`, shadow). Remove the BMD members + the file-global. A/B.
- **6.3 — Lighting group:** `BodyLight` (the ~40-read hot one), `LightEnable`, `ContrastEnable`,
  `ShadowAngle` → `ctx`. Update setup + RenderMesh color path + all Render*Alternative/Translate/
  Shadow variants + the 4 `InstanceRec` builders. A/B (highest visual-regression risk — lighting).
- **6.4 — Animation group:** `BodyAngle`, `CurrentAction`, `PriorAction`, `CurrentAnimation`,
  `CurrentAnimationFrame` → `ctx`. Note the `PriorAction` param-vs-member shadow in `Animation`.
  Update Animation/PlayAnimation + the effect writers (`ZzzEffect.cpp:15205-15210`). A/B.
- **6.5 — Mesh-selection group:** `StreamMesh`, `Skin`, `HideSkin`, `fTransformedSize` → `ctx`.
  Update setup + RenderMesh gates + the cross-TU `HideSkin` reads in `ZzzObject.cpp:7661-8859`. A/B.
- **6.6 — Per-Transform statics → per-worker.** Move `s_lastBoneMatrix`, `s_lastTransformTranslate`,
  `s_lastTransformScale`, `s_transformSerial`, `s_lastLightPosition`, `s_lastLightEnable`,
  `s_skinnedSerial`, `s_meshSkinned[]` into `ctx` (or the per-worker arena) so the "Transform then
  RenderMesh for the same object" correlation holds per-worker. Confirm-dead-and-drop
  `s_skinScratchMin/Max`. A/B.
- **6.7 — Collect globals → per-bucket/per-instance + kill the GL read.** Move `s_instLight`,
  `s_instWave`, `s_chromeWave2`, `s_chromeL`, `s_chromeLightVec` into the `InstanceRec`/per-bucket
  data (like `uvScroll`) OR compute them frame-global outside the parallel region; precompute the
  flat color into `ctx.flatColor` to replace `glGetFloatv(GL_CURRENT_COLOR)` at `ZzzBMD.cpp:1649`.
  A/B (chrome/wave/flat-color visuals).
- **6.8 — Phase-B side-effects → Phase G/serial.** Relocate `CreateBattleCastleCharacter_Visual`
  (`CreateGuardStoneHealingVisual` particle/global writes) to Phase G or a serial post-pass; keep the
  editor pickbox GL draw on the GL thread. Handle the cross-model attach writers
  (`AnimationTransformWithAttachHighModel*` `this->BodyScale = otherObj->Scale`) via `ctx` routing.
  A/B.
- **6.9 — TURN ON PARALLELISM.** Phase B via `ThreadPool::ParallelFor` under `MU_JOBS` (serial below
  a min-entity threshold). Make `BonePaletteTBO::AppendPalette` lock-free (atomic reserve + memcpy).
  Add `[jobs]` profiling (worker count + G/B/F ms). Full race audit pass (TSan if available).
  Measure 100ch + 200ch: build wall-time ÷ workers, `[bmd_cov]` identical on AND off, in-game
  multi-map pixel-identical. **This is the payoff.**

Then the original **Task 7** (objects pass) applies the same to `RenderObjects` if ROI remains.

## Risks (from the map)

1. `PriorAction` param shadows the BMD member in `Animation` — migrate carefully.
2. `BoneScale`: file-global (the per-entity one) vs the rarely-used BMD member — don't conflate.
3. Several fields are written MID-pipeline (`BodyAngle`/`CurrentAnimation*` in Animation, `ShadowAngle`/
   `fTransformedSize` in Transform) → `ctx` must be mutable, not a const setup snapshot.
4. Cross-model attach (`AnimationTransformWithAttachHighModel*`) and effect writers write a BMD's
   per-render state from ANOTHER object's values → route through the correct `ctx`.
5. The per-Transform statics encode a "Transform precedes RenderMesh, same object, no intervening
   Transform" invariant — moving (b) fields without scoping these breaks skin-skip / inst palette-base.
6. Cross-TU reads (`HideSkin` in `ZzzObject.cpp`) widen the blast radius beyond `ZzzBMD.cpp`.
7. Login-scene crash guard: confirm after the member-removal steps (don't assume `sizeof(BMD)` is
   free to change — verify).

## Size & sequencing

**XL.** 9 sub-tasks, each serial-identical + A/B-gated; parallelism flips ON only at 6.9. The member
set is small but reads are pervasive and the static/global coupling must move in lockstep. Execute
subagent-driven (implementer + spec review + code-quality review per sub-task), same as Tasks 1-5.
Estimate the lighting (6.3) and collect-globals (6.7) steps as the highest visual-regression risk →
strongest A/B + in-game on those.
