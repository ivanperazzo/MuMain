#pragma once

// Etapa 3b 6.8b — build emit mode for the parallel character pass.
//
// The per-entity character build (Phase B, BuildVisibleChar -> RenderCharacter) is
// NOT pure: scattered through the render tree it emits visual effects as side effects
// (CreateSprite/CreateParticle/CreateEffect/CreateJoint on global pools) and consumes
// the process-global, non-thread-safe rand()/rand_fps_check(). Several of those rand
// calls run unconditionally on the render path (e.g. the weapon/wing glow Luminosity),
// not just inside effect blocks. Under the parallel Phase B (sub-task 6.9) workers race
// on rand() (CRT rand is not reentrant) and on the global effect pools, so the run is
// non-deterministic: [bmd_cov] jitters (2251-2326) instead of the rock-stable serial 2300.
//
// Rather than per-site deferral across the ~3000-line render tree, we split the pass by
// MODE. The build runs TWICE under MU_JOBS:
//   1. PARALLEL pass in MeshOnly: builds skeleton (Transform/Animation) + emits mesh
//      instances/shadows. ALL non-pure side effects are SUPPRESSED — the four Create*
//      primitives no-op, rand_fps_check() returns false, and Build::Rand() returns 0.
//      So the parallel pass touches NO global effect pool and consumes NO rand -> pure.
//   2. SERIAL replay in EffectsOnly: re-walks RenderCharacter over s_vis IN ENTITY ORDER
//      with mesh-instance/shadow emission SUPPRESSED but Create*/rand restored. Because
//      this drain is serial and ordered, the rand() sequence and the global-pool
//      mutations are EXACTLY what the single serial render produced before -> the visuals
//      (glows, sparks, joints) are byte-identical, just emitted in a separate serial phase.
//
// When MU_JOBS is OFF the mode stays Full (the original single-pass behavior), so the
// serial path is byte-identical to before this change.
//
// Self-contained on purpose (mirrors WorkerArena.h / BmdRenderContext.h): it may be
// linked into the lightweight jobs unit test, so it must NOT pull the engine headers.

namespace Render::Build
{
    enum class EmitMode
    {
        Full,        // single pass: meshes + effects + rand (serial / MU_JOBS off)
        MeshOnly,    // parallel pass: meshes only; Create*/rand suppressed (pure)
        EffectsOnly, // serial replay: effects + rand; mesh-instance/shadow emission suppressed
    };

    // Global (NOT per-worker): set once around each pass on the driving thread, read by
    // the primitives. During the parallel pass every worker observes the same MeshOnly;
    // during the serial replay only one thread runs and observes EffectsOnly.
    void     SetBuildEmitMode(EmitMode m);
    EmitMode GetBuildEmitMode();

    // True when mesh instance / shadow emission must be SUPPRESSED (the EffectsOnly
    // replay). Read at the InstAdd / legacy-draw / shadow-append sites.
    inline bool BuildSuppressMesh() { return GetBuildEmitMode() == EmitMode::EffectsOnly; }

    // True when effect side effects (Create*/rand) must be SUPPRESSED (the MeshOnly
    // parallel pass). Read by the Create* primitives, rand_fps_check, and Build::Rand.
    inline bool BuildSuppressEffects() { return GetBuildEmitMode() == EmitMode::MeshOnly; }

    // Thread-safe rand() shim for the per-entity RENDER path only (NOT the sim). Returns
    // 0 in MeshOnly so the parallel pass consumes no CRT rand (no cross-worker corruption,
    // and the suppressed effects that would have used it are no-ops anyway). In Full /
    // EffectsOnly it forwards to the CRT rand() -> identical sequence to the serial render.
    int Rand();
}
