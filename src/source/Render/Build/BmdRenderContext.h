#pragma once

// Per-worker per-render state being migrated off the shared BMD (group (b) in the
// BMD state map). Today the per-entity render build writes per-render scalar state
// (BodyScale, BodyLight, CurrentAnimation, ...) directly onto the ONE shared
// Models[type] BMD, which races across same-type entities under the job system.
// Etapa 3b moves that mutable state into this per-worker context, reached by
// CurrentRenderCtx() (thread_local indirection via ThreadPool::CurrentWorkerIndex()),
// mirroring Render::Build::WorkerArena exactly. Sub-tasks 6.2-6.9 repoint the BMD
// field setters/readers onto CurrentRenderCtx().field one group at a time.
//
// As of sub-task 6.1 this struct is ALLOCATED (InitRenderCtxs at startup) but
// UNREFERENCED by render code -> trivially serial-identical pure infrastructure.
//
// Self-contained on purpose (mirrors WorkerArena.h): this header may be linked into
// the lightweight jobs unit test, so it must NOT pull "Render/Models/ZzzBMD.h" or
// the "Core/Globals/_types.h" chain. The vec3 fields are spelled as raw float[3]
// (== vec3_t, layout-identical), NOT the engine's vec3_t.

namespace Render::Build
{
    // Per-worker render context. One instance per job worker + the main thread,
    // indexed by Core::Jobs::ThreadPool::CurrentWorkerIndex(). Holds the per-render
    // mutable state that previously lived on the shared BMD; a few fields are
    // written mid-pipeline (Transform/Animation), so this is a live slot, not a
    // const snapshot.
    struct BmdRenderContext
    {
        // placement
        float bodyScale = 1.f;
        float bodyOrigin[3] = {0.f, 0.f, 0.f};
        float bodyHeight = 0.f;
        float boneScale = 1.f;            // the FILE-GLOBAL BoneScale (not the rarely-used BMD member)
        // lighting
        float bodyLight[3] = {0.f, 0.f, 0.f};
        bool  lightEnable = true;
        bool  contrastEnable = false;
        float shadowAngle[3] = {0.f, 0.f, 0.f};   // written mid-pipeline (Transform/lit-build)
        // animation
        float bodyAngle[3] = {0.f, 0.f, 0.f};
        unsigned short currentAction = 0, priorAction = 0;
        float currentAnimation = 0.f;
        short currentAnimationFrame = 0;
        // mesh selection
        char  streamMesh = -1, skin = 0;
        bool  hideSkin = false;
        // outputs
        float fTransformedSize = 0.f;
        // flat color precomputed for the instanced flat branch (replaces glGetFloatv) -- sub-task 6.7
        float flatColor[4] = {1.f, 1.f, 1.f, 1.f};

        // Etapa 3b 6.6: per-Transform correlation context. These encode the
        // "Transform precedes RenderMesh/InstAdd for the SAME object, no intervening
        // Transform" invariant (the P-bmd-skinskip / P-bmd-gpu / instanced-palette-base
        // feature). Previously file-statics in ZzzBMD.cpp, now per-worker so the
        // correlation holds per-worker under parallel Phase B. Initial values mirror the
        // originals exactly (sentinels 0xFFFFFFFF, others 0/false/nullptr).
        float (*lastBoneMatrix)[3][4] = nullptr;   // == ZzzBMD's float(*)[3][4]
        bool  lastTransformTranslate = false;
        float lastTransformScale     = 0.f;
        unsigned transformSerial     = 0;
        float lastLightPosition[3]   = {0.f, 0.f, 0.f};   // == vec3_t
        bool  lastLightEnable        = false;
        unsigned skinnedSerial       = 0xFFFFFFFFu;
        bool  meshSkinned[50]        = {};                // == bool[MAX_MESH]
        // InstPaletteBaseForCurrentPart's function-local statics (instanced palette base
        // cache for the current Transform). Per-worker so parallel workers don't return
        // each other's palette base.
        unsigned instPaletteLastSerial = 0xFFFFFFFFu;
        int      instPaletteLastBase   = 0;
    };

#ifdef MAX_MESH
    static_assert(50 == MAX_MESH, "BmdRenderContext meshSkinned size drifted from MAX_MESH");
#endif

    // Thread-safe across distinct workers ONLY after InitRenderCtxs(>=WorkerCount());
    // the grow path in CtxAt is startup-only and must not run during ParallelFor.
    BmdRenderContext& CurrentRenderCtx();

    // Pre-allocate all worker render contexts (call once at startup; mirrors InitArenas).
    void InitRenderCtxs(int count);
}
