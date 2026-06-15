#include "stdafx.h"

#include "Render/Models/BmdGpuCache.h"

#include "Render/Models/ZzzBMD.h"
#include "Render/Models/BmdInstanceBatch.h"
#include "Render/Models/ShadowInstanceBatch.h"
#include "Render/GL/GLLog.h"
#include "Core/Jobs/ThreadPool.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdlib>

namespace
{
    // Autonomous/headless smoke-tests can't type "$gpubmd on", so honour an env
    // var once: MU_GPUBMD=1 / MU_GPUINST=1 enable the paths at startup.
    bool EnvFlag(const char* name)
    {
        char buf[8] = {};
        size_t n = 0;
        return getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0 && buf[0] == '1';
    }
}

void JobsDiagDumpAndReset();   // 3b-diag: defined in ZzzBMD.cpp (global scope)

namespace Render::Models
{
    namespace
    {
        // One BMD* -> its per-mesh GPU buffers. The vector is sized to NumMeshs the
        // first time the model is touched and never resized again, so MeshGpu
        // addresses stay stable (callers hold a const MeshGpu*). unordered_map node
        // storage also keeps existing entries stable across rehash.
        std::unordered_map<const BMD*, std::vector<MeshGpu>> s_cache;

        bool s_gpuBmdEnabled  = false;   // $gpubmd master switch (default off)
        bool s_gpuObjectsPass = false;   // true only during the Objects render pass
        bool s_gpuCharsPass   = false;   // true only during the Characters render pass
        bool s_gpuInstEnabled = false;   // $gpuinst: instanced Characters batching
        bool s_gpuBlendMesh   = true;    // $gpublendmesh: translucent blend meshes via per-mesh GPU (default ON, Etapa 1.3)
        bool s_gpuBlendInst   = true;    // $gpublendinst: additive translucent blend meshes (wings) -> instanced additive bucket (Etapa 1.4a, default ON, validado in-game 15-jun)
        bool s_gpuWaveInst    = true;    // $gpuwaveinst: textured BRIGHT + UV-scroll (wave) meshes -> instanced additive bucket w/ shader UV offset (Etapa 1.4b, default ON, validado in-game 15-jun)
        bool s_skinSkip       = false;   // $skinskip: Transform skips CPU skinning

        // Etapa 3b 6.9: NoteCharMeshDraw/NoteCharMeshClass run on WORKER threads during
        // the parallel Phase-B build (called from BMD::RenderMesh's collect). Plain int
        // increments would race -> lost updates -> a fluctuating, undercounted [bmd_cov]
        // (the diagnostics only; the actual instanced draws are unaffected). Atomics make
        // the coverage stats deterministic so jobs_off and jobs_on report identically.
        std::atomic<int>  s_charMeshTotal{0};   // chars-pass mesh draws this frame
        std::atomic<int>  s_charMeshGpu{0};     // of those, took the GPU path
        std::atomic<int>  s_visibleChars{0};    // visible characters this frame
        int  s_statFrameCtr   = 0;              // main-thread-only (LogAndResetGpuStats)

        // Coverage breakdown: why each char mesh did/didn't reach the GPU/instanced
        // path. Index = MeshCoverClass. Tells us which legacy bucket to attack next.
        std::atomic<int>  s_charClass[8];       // value-init'd in a startup ctor below

        // Pack the expanded (non-indexed) triangle stream into the interleaved layout
        // BmdShader expects. Mirrors the legacy RenderMesh expansion exactly: for each
        // triangle, for each corner, emit pos/bone/normal/bone/uv in MODEL space.
        // Skinning + lighting move to the shader, so this is built once and resident.
        void BuildMesh(const BMD* model, int meshIndex, int maxBones, MeshGpu& out)
        {
            out.built       = true;
            out.eligible    = false;
            out.vertexCount = 0;

            const Mesh_t* m = &model->Meshs[meshIndex];
            if (m->NumTriangles <= 0 || m->Triangles == nullptr || m->Vertices == nullptr
                || m->Normals == nullptr || m->TexCoords == nullptr)
            {
                return;
            }

            const int expected = m->NumTriangles * 3;
            std::vector<float> verts;
            verts.reserve(static_cast<size_t>(expected) * GpuVtxLayout::kFloats);

            bool boneOk = true;
            for (int j = 0; j < m->NumTriangles; j++)
            {
                const Triangle_t* tri = &m->Triangles[j];
                for (int k = 0; k < tri->Polygon; k++)
                {
                    const int vi = tri->VertexIndex[k];
                    const int ni = tri->NormalIndex[k];
                    const int ti = tri->TexCoordIndex[k];
                    if (vi < 0 || vi >= m->NumVertices
                        || ni < 0 || ni >= m->NumNormals
                        || ti < 0 || ti >= m->NumTexCoords)
                    {
                        return;   // malformed indices -> legacy path
                    }

                    const Vertex_t&   v  = m->Vertices[vi];
                    const Normal_t&   n  = m->Normals[ni];
                    const TexCoord_t& tc = m->TexCoords[ti];

                    if (v.Node < 0 || v.Node >= maxBones || n.Node < 0 || n.Node >= maxBones)
                        boneOk = false;

                    verts.push_back(v.Position[0]);
                    verts.push_back(v.Position[1]);
                    verts.push_back(v.Position[2]);
                    verts.push_back(static_cast<float>(v.Node));
                    verts.push_back(n.Normal[0]);
                    verts.push_back(n.Normal[1]);
                    verts.push_back(n.Normal[2]);
                    verts.push_back(static_cast<float>(n.Node));
                    verts.push_back(tc.TexCoordU);
                    verts.push_back(tc.TexCoordV);
                }
            }

            const int total = static_cast<int>(verts.size() / GpuVtxLayout::kFloats);
            // Only pure triangle meshes (Polygon == 3 everywhere) map 1:1 to a single
            // glDrawArrays of NumTriangles*3; any quad (Polygon == 4) breaks that count
            // -> legacy path. Bone index out of the shader's uBones[] range likewise.
            if (total != expected || !boneOk)
            {
                Render::GL::Log("[bmd_gpu] mesh %p#%d ineligible (total=%d expected=%d boneOk=%d)",
                    static_cast<const void*>(model), meshIndex, total, expected, static_cast<int>(boneOk));
                return;
            }

            out.vbo.Upload(GL_ARRAY_BUFFER, verts.data(),
                static_cast<GLsizeiptr>(verts.size() * sizeof(float)), GL_STATIC_DRAW);
            if (!out.vbo.Valid())
            {
                Render::GL::Log("[bmd_gpu] mesh %p#%d VBO upload failed",
                    static_cast<const void*>(model), meshIndex);
                return;
            }

            out.vertexCount = static_cast<GLsizei>(total);
            out.eligible    = true;
        }
    }

    const MeshGpu* GetOrBuildMeshGpu(const BMD* model, int meshIndex, int maxBones)
    {
        if (model == nullptr || model->Meshs == nullptr
            || meshIndex < 0 || meshIndex >= model->NumMeshs)
        {
            return nullptr;
        }

        // Etapa 3b 6.9 — thread safety under the parallel Phase-B collect. The cache
        // is read concurrently by all workers. Two hazards:
        //   (1) s_cache.emplace structurally modifies the map -> never safe to race.
        //   (2) BuildMesh() issues GL (VBO upload) -> illegal off the main GL thread.
        // The first kWarmupFrames frames run SERIALLY (RenderCharactersClient), so every
        // visible mesh is normally built on the main thread before parallelism kicks in.
        // The lock here is a SAFETY NET for any straggler (an animation-gated mesh whose
        // first sight lands on a later frame): if a worker hits an unbuilt slot we DO NOT
        // build it on the worker (no GL context) — we return nullptr so the collect skips
        // it this frame; the next main-thread frame builds it. The map find/emplace is
        // always taken under the lock so concurrent worker reads never race a rehash.
        static std::mutex s_cacheMtx;
        const bool onWorker = Core::Jobs::ThreadPool::CurrentWorkerIndex() != 0;

        // Etapa 3b 6.9 diagnostic: if a worker ever hits an unbuilt (model,mesh) slot it must
        // defer (return nullptr) -> the collect drops that mesh this frame -> non-deterministic
        // [bmd_cov]. Log it LOUD (once) so any such drop is attributable to a specific
        // (model,mesh). NOTE (6.8b/6.9 investigation): the kJobsWarmupFrames serial warmup
        // makes this NOT the cause of the remaining parallel [bmd_cov] jitter — in the harness
        // this never fires yet inst still jitters, which localised the real race to the shared
        // Models[Type] BMD per-instance render members (BodyAngle/BodyScale/BodyOrigin/Body
        // Light/CurrentAnimation, written by BMD::Animation/Transform) clobbered by concurrent
        // workers building two characters of the same Type. See the 6.8b/6.9 report.
        auto warnWorkerDefer = [&](const char* why) {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                Render::GL::Log("[bmd_gpu] WARN worker-defer (%s): model %p mesh %d UNBUILT on worker %d "
                                "— GPU cache not pre-built on the GL thread; this mesh is DROPPED this "
                                "frame (contributes to non-deterministic [bmd_cov]).",
                                why, static_cast<const void*>(model), meshIndex,
                                Core::Jobs::ThreadPool::CurrentWorkerIndex());
                assert(!"[bmd_gpu] worker hit an unbuilt GPU cache slot (warmup gap)");
            }
        };

        std::unique_lock<std::mutex> lk(s_cacheMtx);
        auto it = s_cache.find(model);
        if (it == s_cache.end())
        {
            if (onWorker)
            {
                warnWorkerDefer("model-miss");
                return nullptr;   // would emplace + build on a worker (GL) -> defer to main thread
            }
            it = s_cache.emplace(model, std::vector<MeshGpu>(model->NumMeshs)).first;
        }

        MeshGpu& slot = it->second[meshIndex];   // node storage stable across rehash -> ref valid after unlock
        if (!slot.built)
        {
            if (onWorker)
            {
                warnWorkerDefer("slot-unbuilt");
                return nullptr;   // unbuilt: building issues GL -> defer to a main-thread frame
            }
            // Main thread: build under the lock (serial warmup / serial frame), so a
            // concurrent worker either sees built==true or gets nullptr (deferred).
            BuildMesh(model, meshIndex, maxBones, slot);
        }
        lk.unlock();

        return &slot;
    }

    void ClearGpuCache()
    {
        s_cache.clear();
    }

    void InvalidateGpuModel(const BMD* model)
    {
        if (model == nullptr)
            return;
        s_cache.erase(model);          // MeshGpu dtors free the VBOs (guarded if GL gone)
        DropInstanceBucketsFor(model); // drop any instance buckets keyed by this BMD*
    }

    void SetGpuBmdEnabled(bool on) { s_gpuBmdEnabled = on; }
    bool GpuBmdEnabled()
    {
        static const bool s_envInit = [] { if (EnvFlag("MU_GPUBMD")) s_gpuBmdEnabled = true; return true; }();
        (void)s_envInit;
        return s_gpuBmdEnabled;
    }
    void SetGpuObjectsPass(bool on) { s_gpuObjectsPass = on; }
    bool GpuObjectsPass()           { return s_gpuObjectsPass; }
    void SetGpuCharsPass(bool on)   { s_gpuCharsPass = on; }
    bool GpuCharsPass()             { return s_gpuCharsPass; }
    void SetGpuInstEnabled(bool on) { s_gpuInstEnabled = on; }
    bool GpuInstEnabled()
    {
        static const bool s_envInit = [] { if (EnvFlag("MU_GPUINST")) s_gpuInstEnabled = true; return true; }();
        (void)s_envInit;
        return s_gpuInstEnabled;
    }

    void SetGpuBlendMeshEnabled(bool on) { s_gpuBlendMesh = on; }
    bool GpuBlendMeshEnabled()
    {
        // Default ON (validated Etapa 1.3). MU_GPUBLENDMESH=0 disables at startup;
        // SetGpuBlendMeshEnabled() toggles at runtime (future antilag panel).
        static const bool s_envInit = [] {
            char b[8] = {}; size_t n = 0;
            if (getenv_s(&n, b, sizeof(b), "MU_GPUBLENDMESH") == 0 && n > 0)
                s_gpuBlendMesh = (atoi(b) != 0);
            return true;
        }();
        (void)s_envInit;
        return s_gpuBlendMesh;
    }

    void SetGpuBlendInstEnabled(bool on) { s_gpuBlendInst = on; }
    bool GpuBlendInstEnabled()
    {
        // Default ON (Etapa 1.4a, validado in-game 15-jun — additive blend meshes/wings
        // collapse into the instanced additive bucket instead of per-mesh GPU). MU_GPUBLENDINST=0
        // disables at startup; SetGpuBlendInstEnabled() toggles at runtime (antilag panel).
        static const bool s_envInit = [] {
            char b[8] = {}; size_t n = 0;
            if (getenv_s(&n, b, sizeof(b), "MU_GPUBLENDINST") == 0 && n > 0)
                s_gpuBlendInst = (atoi(b) != 0);
            return true;
        }();
        (void)s_envInit;
        return s_gpuBlendInst;
    }

    void SetGpuWaveInstEnabled(bool on) { s_gpuWaveInst = on; }
    bool GpuWaveInstEnabled()
    {
        // Default ON (Etapa 1.4b, validado in-game 15-jun — textured BRIGHT meshes with
        // UV-scroll/wave collapse into the instanced additive bucket; the shader applies the
        // per-bucket UV offset). MU_GPUWAVEINST=0 disables at startup; runtime-settable.
        static const bool s_envInit = [] {
            char b[8] = {}; size_t n = 0;
            if (getenv_s(&n, b, sizeof(b), "MU_GPUWAVEINST") == 0 && n > 0)
                s_gpuWaveInst = (atoi(b) != 0);
            return true;
        }();
        (void)s_envInit;
        return s_gpuWaveInst;
    }

    bool GpuSkinDeferEnabled()
    {
        // MU_GPUSKIN=1: defer CPU skin for instanced characters (consumers force-skin
        // lazily). Off by default; the Transform-side gate also requires the char pass +
        // GPU shadows on, so this is just the master flag.
        static const bool s_on = EnvFlag("MU_GPUSKIN");
        return s_on;
    }

    void SetSkinSkip(bool on) { s_skinSkip = on; }
    bool SkinSkip()
    {
        // MU_SKINSKIP=1: skip all CPU skinning (measures the skinning ceiling; legacy
        // non-instanced meshes render wrong, instanced ones are fine — GPU skins from
        // the bone palette, not VertexTransform).
        static const bool s_envInit = [] { if (EnvFlag("MU_SKINSKIP")) s_skinSkip = true; return true; }();
        (void)s_envInit;
        return s_skinSkip;
    }

    void NoteCharMeshDraw(bool wentGpu)
    {
        s_charMeshTotal.fetch_add(1, std::memory_order_relaxed);
        if (wentGpu) s_charMeshGpu.fetch_add(1, std::memory_order_relaxed);
    }

    void NoteCharMeshClass(int cls)
    {
        if (cls >= 0 && cls < 8) s_charClass[cls].fetch_add(1, std::memory_order_relaxed);
    }

    void NoteVisibleChar() { s_visibleChars.fetch_add(1, std::memory_order_relaxed); }

    void LogAndResetGpuStats()
    {
        if (++s_statFrameCtr >= 30)    // ~ every 1-2s depending on FPS
        {
            const int vis = s_visibleChars.load(std::memory_order_relaxed);
            const int tot = s_charMeshTotal.load(std::memory_order_relaxed);
            const int gpu = s_charMeshGpu.load(std::memory_order_relaxed);
            int cc[8];
            for (int i = 0; i < 8; ++i) cc[i] = s_charClass[i].load(std::memory_order_relaxed);
            Render::GL::Log("[bmd_gpu] %d visible chars, %d mesh draws/frame (%d/char), %d via GPU (%d%%) "
                "| inst: %d draws / %d instances | skinskip=%d gpubmd=%d gpuinst=%d",
                vis, tot,
                vis ? (tot / vis) : 0,
                gpu,
                tot ? (gpu * 100 / tot) : 0,
                InstDrawCount(), InstInstanceCount(),
                (int)s_skinSkip, (int)GpuBmdEnabled(), (int)GpuInstEnabled());
            Render::GL::Log("[bmd_cov] inst=%d permeshGPU=%d | legacy: nontex=%d blend=%d wave=%d scale=%d geom=%d other=%d",
                cc[0], cc[1], cc[2], cc[3], cc[4], cc[5], cc[6], cc[7]);
            Render::GL::Log("[bmd_shadow] gpu: %d draws / %d instances (MU_GPUSHADOW=%d)",
                ShadowDrawCount(), ShadowInstanceCount(), (int)GpuShadowEnabled());
            ::JobsDiagDumpAndReset();   // 3b-diag: chars-pass RenderMesh collect tracer (global scope)
            s_statFrameCtr = 0;
        }
        s_charMeshTotal.store(0, std::memory_order_relaxed);
        s_charMeshGpu.store(0, std::memory_order_relaxed);
        s_visibleChars.store(0, std::memory_order_relaxed);
        for (auto& c : s_charClass) c.store(0, std::memory_order_relaxed);
    }
}
