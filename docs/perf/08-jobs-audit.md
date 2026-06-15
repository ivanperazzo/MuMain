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
- `BMD` instance members written during Transform (`BodyScale`, `BodyOrigin`, `BodyLight`,
  `CurrentAction`, `BoneScale`, `ContrastEnable`, …) are written on the **shared
  `Models[Type]` BMD** — `Models[]` is shared across all entities of the same type, so two
  entities of the same model built in parallel would race on these. **Flagged for Task 6:**
  Phase B parallelism requires either copying the per-build BMD scalar state into the
  arena/visible-entry (Task 5's `VisibleChar` is the place) or keying the partition so no
  two same-type entities run concurrently. *(Out of Task 3 scope; this is the primary
  Phase-B hazard Task 5/6 must resolve.)*

## Acceptance grep

After this task, a write to a *file-global* `BoneTransform`/`g_chrome*`/`g_smodels_total`
during the build path no longer exists: the global scratch is renamed to
`g_BoneTransformScratch` (macro → per-worker arena), the chrome caches are arena members,
and `g_smodels_total` is read-only. See the commit for the verifying grep.
