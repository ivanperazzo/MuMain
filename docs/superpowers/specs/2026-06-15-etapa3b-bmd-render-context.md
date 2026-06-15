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

Move all per-render mutable state off the shared `BMD` into a **per-worker `BmdRenderContext`**
(and the coupled file-globals/statics into per-worker homes). After this, building an entity reads
immutable model data from the shared `BMD` and reads/writes only its own worker's context → safe
to run concurrently for entities of the same type.

### Mechanism — per-worker state, NOT param threading (revised 15-jun)

The first implementation attempt threaded a `BmdRenderContext& ctx` parameter through the render
methods. An implementer measurement killed that approach: the BMD render primitives are called from
**~3409 sites across 49 files** (`RenderMesh` 1214, `TransformPosition` 1032, `RenderBody` 539,
`TransformByObjectBone` 405, `Animation` 125, …) — maps, pets, events, quests, AI, effects all use
them. Threading a param means editing all 3409 sites, contradicting "small serial-identical diff".

The right mechanism is the one **Tasks 2-3 already proved**: a **per-worker instance indexed by
`ThreadPool::CurrentWorkerIndex()`**, exactly like `Render::Build::WorkerArena`/`CurrentArena()`.
`BmdRenderContext` lives in a per-worker slot reached by `Render::Build::CurrentRenderCtx()`
(thread_local indirection); **method signatures DO NOT change**. Migration of a field group =
repoint its **setter sites** (`b->member = o->…` → `CurrentRenderCtx().field = o->…`, ~80 across
`ZzzObject`/`ZzzCharacter`/`ZzzEffect`) and its **reader sites** (`this->member` / bare `member`
inside `ZzzBMD.cpp`, ~40) in lockstep, then delete the BMD member. Total per field-group: dozens of
sites in 1-3 files, not thousands.

**Why per-worker (single slot) is correct — not per-entity.** Within one worker everything is
serial and each entity's `set → Transform → RenderMesh` sequence is contiguous (the same
"Transform precedes RenderMesh, same object, no intervening Transform" invariant the per-Transform
statics already rely on). Parallelism is **across** entities on **different** workers, each with its
own slot. This is *exactly* how `g_chrome`/`VertexTransform`/`g_BoneTransformScratch` already behave
after Tasks 2-3 — they are per-worker single slots and render correctly even for bodies with
attached links/wings, which **proves** no "set, render a different-type child that clobbers the slot,
then read" hazard exists on this path. If it did, the Task-2/3 per-worker scratch would already be
visibly broken. So per-worker single-slot reproduces today's shared-member semantics byte-for-byte.
Pre-allocate with `InitRenderCtxs(WorkerCount())` at startup (mirrors `InitArenas`) so the grow path
never runs during `ParallelFor`.

**Hard invariant every sub-task keeps:** serial-identical. `MU_JOBS` stays OFF until the final
switch; every intermediate commit produces pixel-identical output and identical `[bmd_cov]`
(inst=2300 @100ch). Verified by the A/B harness + in-game, exactly like Tasks 1-5.

**CRITICAL — NEVER change `sizeof(BMD)` or the member layout (learned the hard way, 6.3).** The
earlier draft said "moving a member OUT of BMD shrinks it — acceptable." **That is WRONG and caused
a crash.** `Models[]` is allocated as `ModelsDump + rand()%1024` with
`ZeroMemory(Models, MAX_MODELS*sizeof(BMD))` (`ZzzOpenData.cpp:100-102`), and a *pre-existing* heap
overflow (writes the string `"World74\"`) lands on harmless padding **only at the original layout**.
Removing members shifts the array base so the overflow lands on `Models[]`, corrupting `Actions` →
AV in the char render (login-scene crowd crashes; no `bmd_cov`, no MuError entry — silent). 6.2
removed 20 bytes and survived by rand luck; 6.3 removed 26 more and crashed every run.
→ **RULE: do NOT delete or reorder BMD member declarations.** When migrating a field, KEEP its
declaration as **reserved layout padding** (mark it `// reserved (layout) -> ctx.<field>`) and only
repoint its *usage* (reads/writes) to `CurrentRenderCtx()`. The shared BMD becomes effectively
immutable during render (nothing touches the reserved fields) — that is all the parallelism goal
requires; the bytes staying put is free. Re-verify the login-scene crowd harness after every step
(crash is silent in `[bmd_cov]`-only checks).

## The context object

```cpp
// Render/Build/BmdRenderContext.h  (per-WORKER slot, reached by CurrentRenderCtx(); mirror WorkerArena)
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
The render methods read/write `CurrentRenderCtx().field` (mutable: a few fields are written
mid-pipeline) instead of `this->member`. **Signatures are unchanged.** The shared `BMD` keeps only
immutable model data (group (a) in the map). The per-worker slot + accessor + `InitRenderCtxs` live
in `BmdRenderContext.{h,cpp}`, a self-contained pair mirroring `WorkerArena.{h,cpp}` (header
self-contained — `float[3]` not engine `vec3_t` — so it can link into the jobs unit test if needed).

## Surface (from the BMD state map)

~16 per-render members; **BodyLight is the hot one (~40 reads)**, BodyOrigin ~18, BodyScale ~16,
LightEnable ~12, anim fields ~3-9 each, StreamMesh/HideSkin ~6-10. **No method signatures change.**
Per field group, repoint its reader sites (mostly inside `ZzzBMD.cpp`: `this->member`/bare `member`)
and its setter sites (~80 `b->member = o->…` total across `ZzzObject.cpp`, `ZzzCharacter.cpp`,
`ZzzEffect.cpp`) to `CurrentRenderCtx().field`, then delete the BMD member. Coupled non-member
state: file-global `BoneScale`, the per-Transform statics
(`s_lastBoneMatrix`/`s_transformSerial`/`s_meshSkinned[]`/…), the `InstSet*` collect globals, and
the `glGetFloatv(GL_CURRENT_COLOR)` build-path GL read.

## Decomposition — serial-identical sub-tasks (each its own commit + A/B)

Each step moves one cohesive group from `BMD` member → `CurrentRenderCtx().field`, updating ALL
write/read sites in lockstep (a field can't be half-migrated — its ~40 read sites + setters must
move together). **No method signatures change.** After each step: build Release,
`run-harness-ab.bat 1 1 <tag>` → `[bmd_cov]` identical, in-game spot-check.

- **6.1 — Add the per-worker `BmdRenderContext` infrastructure (no field moved yet).** Create
  `Render/Build/BmdRenderContext.{h,cpp}`: the struct (self-contained header, `float[3]`), a
  per-worker slot vector + `CurrentRenderCtx()` accessor + `InitRenderCtxs(int)`, mirroring
  `WorkerArena.{h,cpp}` line-for-line (same `std::vector<unique_ptr>` + `CurrentWorkerIndex()` +
  assert-after-init pattern). Call `InitRenderCtxs(WorkerCount())` at startup right next to the
  existing `InitArenas(...)` (`Winmain.cpp`). Wire into `src/CMakeLists.txt` if needed (GLOB should
  pick it up). **Nothing reads or writes the ctx yet** → trivially serial-identical (pure infra).
  No BMD member removed, no signature touched.
- **6.2 — Placement group:** `BodyScale`, `BodyOrigin`, `BodyHeight`, file-global `BoneScale` →
  `CurrentRenderCtx()`. Repoint setters + ~50 read sites (SkinMesh, Transform, TransformPosition,
  lit-build, inst gate `:1553-1581`, shadow). RETAIN the BMD members as reserved padding (do NOT
  delete — sizeof(BMD) must stay byte-identical); remove only the file-global `BoneScale` (not a
  BMD member, so it doesn't affect layout). A/B + login-scene crowd harness (crash is silent).
- **6.3 — Lighting group:** `BodyLight` (the ~40-read hot one), `LightEnable`, `ContrastEnable`,
  `ShadowAngle` → `CurrentRenderCtx()`. Repoint setters + RenderMesh color path + all
  Render*Alternative/Translate/Shadow variants + the 4 `InstanceRec` builders. A/B (highest
  visual-regression risk — lighting).
- **6.4 — Animation group:** `BodyAngle`, `CurrentAction`, `PriorAction`, `CurrentAnimation`,
  `CurrentAnimationFrame` → `CurrentRenderCtx()`. Note the `PriorAction` param-vs-member shadow in
  `Animation`. Repoint Animation/PlayAnimation + the effect writers (`ZzzEffect.cpp:15205-15210`).
  A/B.
- **6.5 — Mesh-selection group:** `StreamMesh`, `Skin`, `HideSkin`, `fTransformedSize` →
  `CurrentRenderCtx()`. Repoint setters + RenderMesh gates + the cross-TU `HideSkin` reads in
  `ZzzObject.cpp:7661-8859`. A/B.
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
   `fTransformedSize` in Transform) → the per-worker ctx is mutable (it is a live slot, not a const snapshot).
4. Cross-model attach (`AnimationTransformWithAttachHighModel*`) and effect writers write a BMD's
   per-render state from ANOTHER object's values → with the per-worker mechanism these become
   `CurrentRenderCtx().field = otherObj->…`, which automatically targets the current worker's slot
   (the contiguous set→use guarantee makes this correct without extra routing).
5. The per-Transform statics encode a "Transform precedes RenderMesh, same object, no intervening
   Transform" invariant — this is the SAME invariant that makes the per-worker single-slot ctx
   correct; 6.6 moves these statics to the same per-worker home so the whole group stays consistent.
6. Cross-TU reads (`HideSkin` in `ZzzObject.cpp`) widen the blast radius beyond `ZzzBMD.cpp`.
7. Login-scene crash guard: confirm after the member-removal steps (don't assume `sizeof(BMD)` is
   free to change — verify).

## Size & sequencing

**XL.** 9 sub-tasks, each serial-identical + A/B-gated; parallelism flips ON only at 6.9. The member
set is small but reads are pervasive and the static/global coupling must move in lockstep. Execute
subagent-driven (implementer + spec review + code-quality review per sub-task), same as Tasks 1-5.
Estimate the lighting (6.3) and collect-globals (6.7) steps as the highest visual-regression risk →
strongest A/B + in-game on those.
