#pragma once

// Self-contained on purpose: this header is linked into the lightweight jobs unit
// test (test_worker_arena), so it must NOT pull "Render/Models/ZzzBMD.h" or the
// "Core/Globals/_types.h" chain — both are only compilable behind the engine's
// precompiled stdafx (they need OBJECT, VectorCopy, BYTE, std::wstring, DWORD, ...).
// We therefore spell the element type as a raw float[3] (== vec3_t, layout-identical)
// and mirror ZzzBMD.h's size constants, guarded by a static_assert that fires in any
// real engine translation unit (where MAX_MESH / MAX_VERTICES are already defined).

namespace Render::Build
{
    // Single source of truth stays ZzzBMD.h; the static_asserts below catch drift.
    inline constexpr int kArenaMaxMesh     = 50;
    inline constexpr int kArenaMaxVertices = 15000;

#ifdef MAX_MESH
    static_assert(kArenaMaxMesh == MAX_MESH, "WorkerArena MAX_MESH drifted from ZzzBMD.h");
#endif
#ifdef MAX_VERTICES
    static_assert(kArenaMaxVertices == MAX_VERTICES, "WorkerArena MAX_VERTICES drifted from ZzzBMD.h");
#endif

    // Per-worker transient skin scratch. One instance per job worker + the main
    // thread, indexed by Core::Jobs::ThreadPool::CurrentWorkerIndex(). ~30 MB each;
    // never per-entity (per-entity would be GBs). Replaces the old file-global
    // VertexTransform/NormalTransform/IntensityTransform/LightTransform/g_chrome.
    // float[N][3] is layout-identical to the old vec3_t[N], so every call site and
    // function-argument decay (vec3_t(*)[MAX_VERTICES]) stays binary-compatible.
    struct WorkerArena
    {
        float vertexTransform[kArenaMaxMesh][kArenaMaxVertices][3];
        float normalTransform[kArenaMaxMesh][kArenaMaxVertices][3];
        float intensityTransform[kArenaMaxMesh][kArenaMaxVertices];
        float lightTransform[kArenaMaxMesh][kArenaMaxVertices][3];
        float chrome[kArenaMaxVertices][2];
    };

    // Arena for the calling thread. Heap-allocated lazily, kept for process life.
    WorkerArena& CurrentArena();

    // Pre-allocate all worker arenas (call once at startup; avoids first-frame stalls).
    void InitArenas(int count);
}

// Per-vertex skin scratch now lives in the per-worker arena (Task 2). These macros
// keep every existing VertexTransform[i][j]-style call site unchanged. Any TU that
// touched the old file-globals includes this header instead of declaring the
// matching `extern`.
#define VertexTransform     (Render::Build::CurrentArena().vertexTransform)
#define NormalTransform     (Render::Build::CurrentArena().normalTransform)
#define IntensityTransform  (Render::Build::CurrentArena().intensityTransform)
#define LightTransform      (Render::Build::CurrentArena().lightTransform)
#define g_chrome            (Render::Build::CurrentArena().chrome)
