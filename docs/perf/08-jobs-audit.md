# 08 — Jobs reentrancy audit (Etapa 3, Task 3)

> Goal of Task 3: after this commit the **per-entity render-build path**
> (`RenderCharacter` / `Calc_RenderObject` → `BMD::Transform` → `BMD::Animation`)
> writes **no shared mutable file-global**. Every build-time global is resolved to one
> of four buckets:
> **per-worker** (transient scratch → `Render::Build::WorkerArena`, indexed by
> `ThreadPool::CurrentWorkerIndex()`), **per-entity** (durable pose → `OBJECT::BoneTransform`),
> **read-only** (snapshot, never written in the build), or **moved to Phase G**
> (cross-entity decision pulled out of the per-entity loop body; Task 5/6 relocate it).
>
> Serial-identical: this is a pure relocation/rename. Flag OFF and ON produce
> identical `[bmd_cov]` and pixels vs Task 2.

## How the bone palette actually works in this codebase (key finding)

The client **already** has a per-entity bone-palette mechanism that predates the jobs work:

- `OBJECT::BoneTransform` is a `vec34_t*` (`w_ObjectInfo.h:216`) — a per-entity, durable
  bone matrix array. `OBJECT::EnableBoneMatrix` (`w_ObjectInfo.h:130`) selects it.
- **Characters and pets already set `EnableBoneMatrix = true` and allocate
  `o->BoneTransform = new vec34_t[NumBones]`** (`ZzzCharacter.cpp:11681,11897`;
  `CSPetSystem.cpp:96,159`). So `Calc_RenderObject`/`RenderCharacter` already build the
  durable skeleton **into the per-entity pointer** (`ZzzObject.cpp:287-290, 363-366` take
  the `o->EnableBoneMatrix` branch → `b->Animation(o->BoneTransform,…)` /
  `b->Transform(o->BoneTransform,…)`). `BMD::Animation`/`BMD::Transform`/`BMD::SkinMesh`
  all write into their **`BoneMatrix` parameter**, never the global.
- The downstream consumers (`InstAppendPalette`, legacy draw, shadow, physics, effects'
  attach-to-bone) read `o->BoneTransform[bone]` (the per-entity pointer) — confirmed by
  the 32 files using `->BoneTransform` member access.

→ The per-entity publish point the plan asks for **already exists**. The remaining work
is to make the **global `BoneTransform` scratch** (used only when `EnableBoneMatrix` is
false, i.e. transient build scratch for effects / map props / pet fallback) per-worker,
and to isolate the chrome caches + `g_smodels_total` + the cloaking cross-entity write.

## Globals written during the build — enumeration + resolution

| Global | Where defined | Read/write during build | Resolution |
|---|---|---|---|
| `BoneTransform[MAX_BONES][3][4]` | `ZzzBMD.cpp:37`, extern `ZzzBMD.h:339` | Transient scratch: `Model->Animation(BoneTransform,…)` then `Model->TransformPosition(BoneTransform[N],…)` at ~789 bare call sites across effects, AI, game-maps, pets, MonkSystem. Used **only** when `o->EnableBoneMatrix==false`; characters/pets use `o->BoneTransform` instead. | **per-worker** → `WorkerArena::boneScratch`. Cannot be an object-like macro named `BoneTransform` (collides with the `OBJECT::BoneTransform` *member* used at 32 files / `pObject->BoneTransform`). Resolved by **renaming the file-global symbol + its 789 bare uses to `g_BoneTransformScratch`**, which is `#define`-mapped to `Render::Build::CurrentArena().boneScratch`. The `->BoneTransform` member access is left untouched (it is the per-entity durable palette). |
| `g_chromeage[MAX_BONES]` | `ZzzBMD.cpp:745` (file-local, no extern) | `BMD::Chrome` writes `g_chromeage[bone] = g_smodels_total`. Per-bone chrome cache, used only inside `ZzzBMD.cpp`. | **per-worker** → `WorkerArena::chromeAge`, macro-mapped (ZzzBMD.cpp-only). |
| `g_chromeup[MAX_BONES]` | `ZzzBMD.cpp:746` (file-local) | `BMD::Chrome` reads it (`DotProduct(normal, g_chromeup[bone])`). Per-bone chrome cache. | **per-worker** → `WorkerArena::chromeUp`. |
| `g_chromeright[MAX_BONES]` | `ZzzBMD.cpp:747` (file-local) | `BMD::Chrome` reads it. Per-bone chrome cache. | **per-worker** → `WorkerArena::chromeRight`. |
| `g_smodels_total` | `ZzzBMD.cpp:743` (file-local) | Monotonic "frame id" cookie meant to invalidate the chrome cache; **read** in `BMD::Chrome` (`g_chromeage[bone] = g_smodels_total`). **Never incremented anywhere** (grep confirms one def + one read in the whole tree; it is a constant `1`, the original cache-invalidation was dead). | **read-only** snapshot. Kept as a single read-only `int` (never written in the build path). No per-entity build path mutates it, so no relocation is needed; documented here so Task 6 does not reintroduce a write. |
| `BoneQuaternion[MAX_BONES]` (`vec4_t`) | `ZzzBMD.cpp:32` (file-global, no extern; ZzzBMD.cpp-only) | `BMD::Animation`'s per-bone slerp workspace: written via `QuaternionSlerp`/`QuaternionCopy` (~:118,:122) then consumed by `QuaternionMatrix` (~:126) in the same bone loop. Transient per-bone scratch, never read across calls. | **per-worker** → `WorkerArena::boneQuaternion[][4]` (`vec4_t == float[4]`, guarded by `static_assert`). Macro-mapped file-locally in ZzzBMD.cpp (like the chrome caches). |
| `ParentMatrix[3][4]` | `ZzzBMD.cpp:55` (file-global) + `extern` in `ZzzCharacter.cpp:6513` | `BMD::Animation` root-bone branch (`AngleMatrix`/scale/origin then `R_ConcatTransforms(ParentMatrix, Matrix, BoneMatrix[i])`, ~:147-163), `BMD::RotationPosition` (~:477 copies `Matrix` into it — write-only sink, never read back), and `RenderGuild`/effect bone-attach in ZzzCharacter.cpp as a throwaway `R_ConcatTransforms` target then immediate read. Transient scratch, no cross-call carry. | **per-worker** → `WorkerArena::parentMatrix[3][4]`. Cross-TU, so the macro lives in `WorkerArena.h` (global macro, like `g_BoneTransformScratch`); the `extern` in ZzzCharacter.cpp is dropped (header is already pulled in via ZzzBMD.h). |
| `g_vright` (`vec3_t`) | `ZzzBMD.cpp:749` (file-global, no extern) | Written to the invariant constant `(0,0,1)` at the top of `BMD::Chrome` (~:760), read within the same function (`CrossProduct(tmp, g_vright, …)`, ~:770). Benign (write-then-read of a constant) but still a shared mutable global. | **read-only/benign → made function-local.** Demoted to a `vec3_t` local in `BMD::Chrome` (set per call to the same constant). Used nowhere else, so it is no longer a global at all — no shared state, no race. |
| `g_chromeage[bone] = g_smodels_total` (the *write*, `BMD::Chrome` ~:775) | `ZzzBMD.cpp` | The only access to `g_chromeage` in the whole tree is this write; **no branch reads it** (grep). Since `g_smodels_total` is `const 1`, it stores a constant nothing consumes. | **vestigial — retained with a comment.** Left in place (serial-identical safety) with an inline note that the chrome-cache invalidation it keyed is dead; Task 6 must not reintroduce a reader. The arena member `chromeAge` stays so the write target exists. |
| `Hero->TargetCharacter = -1` | `ZzzCharacter.cpp:11360-11391` | Cloaking block in `RenderCharactersClient`'s per-entity loop: clears the Hero's lock-on if the now-hidden (battle-castle cloaked) entity was the target, then `continue`s (skips rendering that entity). **Cross-entity mutation** of a *different* object (`Hero`) from inside the per-entity loop body → cannot run in parallel Phase B. | **moved to Phase G** (extracted now): pulled into a named helper `ClearCloakedTarget(CHARACTER* c)` returning whether to skip the entity. Called serially from the same place now (behavior-neutral); Task 5 relocates the call to Phase G (the serial gather), Task 6 keeps it out of parallel Phase B. |

## Globals touched during the build that are already safe (no change)

These are written/read during `Transform`/`Animation` but are **per-entity-derived
read-only-during-collect** or **single-threaded-by-construction**, and are documented
so Task 6's race-audit pass can tick them off:

- `OBJECT::BoneTransform` (per-entity pointer) — **per-entity**, one entity per worker
  in Phase B; no aliasing across entities. The durable palette the GPU TBO/legacy draw read.
- `VertexTransform / NormalTransform / IntensityTransform / LightTransform / chrome`
  (`g_chrome`) — already moved to `WorkerArena` in **Task 2** (per-worker).
- `s_transformSerial`, `s_lastBoneMatrix`, `s_lastTransformTranslate`, `s_lastTransformScale`,
  `s_lastLightPosition`, `s_lastLightEnable`, `s_skinnedSerial`, `s_meshSkinned[]`
  (`ZzzBMD.cpp:178-194`) — the P-bmd-skinskip / instanced-palette **per-Transform context
  statics**. These are written by `Transform` and read by the immediately-following
  `RenderMesh`/shadow with no intervening `Transform` (same-object invariant). **Still
  shared mutable file-globals.** They are NOT covered by this task. **Flagged for Task 4/6:**
  to run Phase B in parallel they must become per-worker (they are transient per-Transform,
  so per-worker arena is the natural home) OR the skin-skip path must be disabled under
  `MU_JOBS`. Recorded here so the Task 6 race-audit does not miss them. *(Out of Task 3
  scope — Task 3 covers the explicitly-named `BoneTransform`/chrome/`g_smodels_total`/cloak.)*
- `BMD` instance members written during `Animation`/`Transform` on the **shared
  `Models[Type]` BMD** — `Models[]` is shared across all entities of the same type, so two
  entities of the same model built in parallel would race on these. Explicit list of the
  most-written members (no elision):
  - **`CurrentAnimation`, `CurrentAnimationFrame`, `CurrentAction`, `PriorAction`, `BodyAngle`**
    — all written at the top of `BMD::Animation` (~:64-70: `VectorCopy(Angle, BodyAngle)`,
    `CurrentAnimation = AnimationFrame`, `CurrentAnimationFrame = (int)…`, plus the
    `PriorAction`/`CurrentAction` clamps).
  - **`BodyScale`, `BodyOrigin`, `BodyLight`, `BoneScale`** (the BMD member, distinct from the
    *file-global* `BoneScale` below), **`ContrastEnable`** — set by the Transform/Animation
    setup before the bone loop / chrome.
  **Flagged for Task 6:** Phase B parallelism requires either copying the per-build BMD
  scalar state into the arena/visible-entry (Task 5's `VisibleChar` is the place) or keying
  the partition so no two same-type entities run concurrently. *(Out of Task 3 scope; this
  is the primary Phase-B hazard Task 5/6 must resolve.)*
- **`BoneScale` (file-global, `ZzzBMD.cpp:187`)** — NEW find during the chain re-walk; not
  previously listed. Written **on the per-entity build path** in `Calc_RenderObject`
  (`ZzzObject.cpp:296-360`, right after `b->Animation(...)`, branching on the entity's edge
  /select scale) and read later in `BMD::SkinMesh` (`ZzzBMD.cpp:223`) and the instanced-skin
  gates (`:1553`, `:2874`). A genuine shared mutable scalar driven per-entity → two entities
  of different scale built in parallel would race. **Flagged for Task 6** (same resolution as
  the `Models[Type]` scalars: hoist into the per-entity visible-entry/arena, or partition by
  type+scale). *(Out of Task 3 scope — Task 3 covers only the named `BoneQuaternion`/
  `ParentMatrix`/`g_vright`/chrome/`g_smodels_total`/`BoneTransform`/cloak.)*
- `s_skinScratchMin`, `s_skinScratchMax` (`ZzzBMD.cpp:211`, file-scope statics) — the bounding
  sink for `BMD::SkinMesh`'s lazy-skin call at `:288`; commented "unused in gameplay". Written
  but their result is discarded. Benign today (dead output), but still shared mutable statics:
  **Flagged for Task 6** to confirm-dead-and-drop or make per-worker, so the race audit is
  exhaustive. *(Out of Task 3 scope.)*
- `g_vright` (`BMD::Chrome`) — was a file-global; **resolved in this task** (demoted to a
  function-local, see the enumeration table above). No longer shared state.
- **`s_instLight`, `s_instWave`, `s_chromeWave2`, `s_chromeL[3]`, `s_chromeLightVec[3]`
  (`BmdInstanceBatch.cpp:52-56`)** — NEW find during the **Task 4 review**. These instanced
  collect globals are written **per-mesh** from inside the per-entity build via
  `InstSetLight` (`ZzzBMD.cpp:1633,1643`), `InstSetWave` (`:1597,1612`), and
  `InstSetChromeParams` (`:1616`), and read once at `InstFlush`. The design treats them as
  "frame-global" but they are set per mesh and only converge because the build is serial.
  Under Task 6 parallelism multiple workers race-write them and the flush reads a
  non-deterministic last-writer → breaks serial-identical chrome/wave/lit visuals.
  **Flagged for Task 6:** move them into the per-instance `InstanceRec` / per-bucket data
  (exactly like `uvScroll` already was), OR hoist their computation to a true frame-global
  outside the parallel region. *(Out of Task 4 scope — Task 4 keeps the build serial.)*
- **`glGetFloatv(GL_CURRENT_COLOR, cur)` (`ZzzBMD.cpp:1649`, flat-color instanced branch)** —
  a **GL read on the build path**, illegal off the GL thread. Under Task 6 the build runs on
  worker threads with no GL context → this call is invalid. **Flagged for Task 6:** precompute
  the flat color into the per-entity visible-entry (Task 5) / `InstanceRec` so no GL state is
  read during the parallel build. *(Pre-existing; surfaced by the Task 4 review.)*
- **Phase-B non-render side-effects inside `BuildVisibleChar` (Task 5 review finds):**
  - **`CreateBattleCastleCharacter_Visual(c,o)` → `CreateGuardStoneHealingVisual`
    (`GMBattleCastle.cpp:1734-1766`)** — LIVE (non-editor) shared-mutable write on the Phase-B
    path: reads/writes globals `LastHealingParticle` + `WorldTime` and calls `CreateParticle`
    (mutates the global particle pool). Under parallelism workers race on these. **Flagged for
    Task 6:** move this per-entity side-effect to Phase G (serial), or run it in a serial
    post-pass after the parallel build. Higher priority — it is not editor-gated.
  - **`RenderCharacterPickBoxDebug(o)` (`ZzzCharacter.cpp:91`, `_EDITOR` only)** — issues GL
    directly; illegal off the GL thread. Editor-only, so a Release `MU_JOBS` build is unaffected,
    but documented for completeness. **Flagged for Task 6:** keep editor pickbox draws on the
    GL thread (Phase F / serial).

## Acceptance grep

After this task, a write to a *file-global* `BoneTransform`/`BoneQuaternion`/`ParentMatrix`/
`g_chrome*`/`g_vright`/`g_smodels_total` during the build path no longer exists: the
hierarchy-concat scratch is renamed to `g_BoneTransformScratch`, `BoneQuaternion` and
`ParentMatrix` are arena members (macro-mapped), the chrome caches are arena members,
`g_vright` is now a function-local, and `g_smodels_total` is read-only. See the commit for
the verifying grep.

> **Chain re-walk (review follow-up).** The full
> `RenderCharacter`→`Calc_RenderObject`→`BMD::Animation`→`BMD::Transform`→`BMD::Chrome`→
> `RenderMesh`/`InstAdd` chain was re-walked for this commit. Beyond the three globals the
> review flagged (`BoneQuaternion`, `ParentMatrix`, `g_vright`), the re-walk surfaced **one
> additional genuine Phase-B hazard not previously listed — the file-global `BoneScale`**
> (`ZzzObject.cpp` writes it per-entity, `SkinMesh`/instanced gates read it) — plus the dead
> `s_skinScratchMin/Max` sinks. Both are now documented in the "already-safe / Task-6" section.
> The remaining shared mutable state on the per-entity path is the per-`Transform` statics
> (`s_lastBoneMatrix`, `s_transformSerial`, `s_meshSkinned[]`, …) and the shared `Models[Type]`
> scalar members (now fully enumerated) — all owned by Task 4/5/6, not Task 3.
