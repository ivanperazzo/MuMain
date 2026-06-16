#include "stdafx.h"

#include "Render/Models/ShadowInstanceBatch.h"

#include "Render/Models/BmdGpuCache.h"
#include "Render/GL/BonePaletteTBO.h"
#include "Render/GL/BmdShader.h"
#include "Render/GL/InstancedShadowShader.h"
#include "Render/GL/GpuBuffer.h"
#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Core/Jobs/ThreadPool.h"

#include <gl/glew.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace Render::Models
{
    namespace
    {
        // Per-instance VBO layout (must match InstancedShadowShader attribs).
        // 6 floats: paletteBase(1) bodyScale(1) bodyOrigin(3) groundZ(1).
        constexpr GLsizei kInstFloats = 6;
        constexpr GLsizei kInstStride = kInstFloats * (GLsizei)sizeof(float);
        constexpr GLsizei kOffBase    = 0;
        constexpr GLsizei kOffScale   = 1 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffOrigin  = 2 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffGround  = 5 * (GLsizei)sizeof(float);

        // GL-free per-worker payload (model/mesh + flattened recs). Merged at flush.
        struct ShadowBucketData
        {
            const BMD* model = nullptr;
            int        meshIndex = 0;
            std::vector<float> recs;   // flattened ShadowRec (6 floats each)
        };
        // GL-backed draw bucket: the merged payload + a per-bucket VBO (reused across frames).
        struct Bucket : ShadowBucketData
        {
            Render::GL::GpuBuffer  instVbo;
        };
        using ShadowBucketMap = std::unordered_map<uint64_t, ShadowBucketData>;

        // Etapa 3b 6.9: per-worker collection so the (parallel) Phase-B shadow collect
        // never mutates a shared map. Each worker appends into its OWN map; flush merges
        // them (order-independent: every shadow is the same 50%-black stencil-incr draw,
        // so within-key concat order does not change pixels). Serial path = worker 0 only
        // => byte-identical to the pre-6.9 single-map behaviour.
        std::vector<ShadowBucketMap>         s_workerBuckets;
        std::unordered_map<uint64_t, Bucket> s_drawBuckets;
        int   s_drawCount = 0;
        int   s_instCount = 0;
        // sx/sy are map-global (identical for every char this frame); written by every
        // ShadowAdd to the same value, so a relaxed atomic last-writer is race-free.
        std::atomic<float> s_sx{2000.f};
        std::atomic<float> s_sy{4000.f};

        int WorkerBucketCount()
        {
            const int n = Core::Jobs::ThreadPool::Instance().WorkerCount();
            return n < 1 ? 1 : n;
        }

        uint64_t Key(const BMD* m, int mesh)
        {
            return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)) << 16)
                 ^ static_cast<uint64_t>(mesh & 0xFFFF);
        }
    }

    bool GpuShadowEnabled()
    {
        static const bool s_on = [] {
            char buf[8] = {}; size_t n = 0;
            return getenv_s(&n, buf, sizeof(buf), "MU_GPUSHADOW") == 0 && n > 0 && buf[0] == '1';
        }();
        return s_on;
    }

    void ShadowBegin()
    {
        const int workers = WorkerBucketCount();
        if ((int)s_workerBuckets.size() < workers)
            s_workerBuckets.resize(workers);
        for (auto& wm : s_workerBuckets)
            for (auto& kv : wm)
                kv.second.recs.clear();
        s_drawCount = 0;
        s_instCount = 0;
    }

    void ShadowAdd(const BMD* model, int meshIndex, const ShadowRec& rec, float sx, float sy)
    {
        s_sx.store(sx, std::memory_order_relaxed);   // map-global, identical for all chars this frame
        s_sy.store(sy, std::memory_order_relaxed);
        // Append to the CALLING worker's map (no shared mutation). Serial path => worker 0.
        const int w = Core::Jobs::ThreadPool::CurrentWorkerIndex();
        ShadowBucketMap& wm = s_workerBuckets[(w >= 0 && w < (int)s_workerBuckets.size()) ? w : 0];
        ShadowBucketData& b = wm[Key(model, meshIndex)];
        b.model = model; b.meshIndex = meshIndex;
        b.recs.push_back(rec.paletteBase);
        b.recs.push_back(rec.bodyScale);
        b.recs.push_back(rec.bodyOrigin[0]);
        b.recs.push_back(rec.bodyOrigin[1]);
        b.recs.push_back(rec.bodyOrigin[2]);
        b.recs.push_back(rec.groundZ);
    }

    void ShadowFlush()
    {
        using namespace Render::GL;
        InstancedShadowShader& sh = GetInstancedShadowShader();
        BonePaletteTBO&        tbo = GetBonePaletteTBO();
        if (!sh.Ensure() || !tbo.Ensure())
            return;

        // Merge the per-worker collections into the draw set (order-independent — every
        // shadow is the same 50%-black stencil-incr draw). Retain s_drawBuckets's GpuBuffers
        // across frames (keyed VBOs reused); clear only the records. Workers are visited in
        // index order so the serial (worker-0-only) path is byte-identical.
        for (auto& kv : s_drawBuckets)
            kv.second.recs.clear();
        for (int w = 0; w < (int)s_workerBuckets.size(); ++w)
        {
            for (auto& kv : s_workerBuckets[w])
            {
                if (kv.second.recs.empty())
                    continue;
                Bucket& d = s_drawBuckets[kv.first];
                d.model = kv.second.model;
                d.meshIndex = kv.second.meshIndex;
                d.recs.insert(d.recs.end(), kv.second.recs.begin(), kv.second.recs.end());
            }
        }

        // The body InstFlush already uploaded the palette this frame; re-bind it.
        // (Upload is idempotent; calling it here makes the pass order-independent in
        // case no body instances flushed.)
        tbo.Upload();

        while (glGetError() != GL_NO_ERROR) {}

        sh.Use();
        tbo.Bind(1);
        sh.SetPaletteUnit(1);
        sh.SetSkew(s_sx.load(std::memory_order_relaxed), s_sy.load(std::memory_order_relaxed));
        const float kShadowColor[4] = { 0.f, 0.f, 0.f, 0.5f };   // legacy: 50% black
        sh.SetColor(kShadowColor);

        const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone();
        const GLint iBase = sh.AttrPaletteBase(), iScale = sh.AttrBodyScale(),
                    iOrig = sh.AttrBodyOrigin(), iGround = sh.AttrGroundZ();

        // Replicate RenderBodyShadow's GL state THROUGH the cached state helpers
        // (EnableAlphaTest/DisableTexture/DisableDepthMask) — NOT raw glEnable/glDepthMask.
        // The helpers track AlphaBlendType/TextureEnable/depth-mask in statics to elide
        // redundant GL calls; bypassing them with raw calls desyncs the cache, so a later
        // DisableDepthMask()/EnableAlphaBlend() (e.g. the elf-wing additive effect) becomes
        // a no-op against stale cache -> wrong depth-mask/blend -> screen flicker.
        // EnableAlphaTest(false): blend = SRC_ALPHA/ONE_MINUS_SRC_ALPHA (50% black over bg),
        // alpha test on; depth test stays on so shadows are occluded by geometry in front.
        EnableAlphaTest(false);
        DisableTexture();      // shader outputs solid uColor; keep the texture cache in sync
        DisableDepthMask();    // shadows must not write depth
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

        namespace L = Render::Models::GpuVtxLayout;
        for (auto& kv : s_drawBuckets)
        {
            Bucket& b = kv.second;
            const int instances = (int)(b.recs.size() / kInstFloats);
            if (instances == 0)
                continue;

            const MeshGpu* g = GetOrBuildMeshGpu(b.model, b.meshIndex, BmdShader::kMaxBones);
            if (g == nullptr || !g->eligible)
                continue;

            // Shared geometry (divisor 0): only position + vertex bone are needed.
            g->vbo.Bind(GL_ARRAY_BUFFER);
            if (aPos >= 0) { EnableVertexAttribArray(aPos); VertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffPos);   VertexAttribDivisor(aPos, 0); }
            if (aVB  >= 0) { EnableVertexAttribArray(aVB);  VertexAttribPointer(aVB,  1, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffVBone); VertexAttribDivisor(aVB, 0); }

            // Per-instance data (divisor 1).
            b.instVbo.Upload(GL_ARRAY_BUFFER, b.recs.data(),
                (GLsizeiptr)(b.recs.size() * sizeof(float)), GL_DYNAMIC_DRAW);
            b.instVbo.Bind(GL_ARRAY_BUFFER);
            if (iBase   >= 0) { EnableVertexAttribArray(iBase);   VertexAttribPointer(iBase,   1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffBase);   VertexAttribDivisor(iBase, 1); }
            if (iScale  >= 0) { EnableVertexAttribArray(iScale);  VertexAttribPointer(iScale,  1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffScale);  VertexAttribDivisor(iScale, 1); }
            if (iOrig   >= 0) { EnableVertexAttribArray(iOrig);   VertexAttribPointer(iOrig,   3, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffOrigin); VertexAttribDivisor(iOrig, 1); }
            if (iGround >= 0) { EnableVertexAttribArray(iGround); VertexAttribPointer(iGround, 1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffGround); VertexAttribDivisor(iGround, 1); }

            DrawArraysInstanced(GL_TRIANGLES, 0, g->vertexCount, instances);
            ++s_drawCount;
            s_instCount += instances;
        }

        if (s_drawCount > 0)
        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                Render::GL::Log("[bmd_shadow] GL error 0x%x after %d shadow draws", (unsigned)err, s_drawCount);
        }

        // Restore: disable attribs/divisors, restore the opaque end-state downstream
        // legacy draws expect (depth write on, stencil off, alpha test on).
        const GLint attrs[] = { aPos, aVB, iBase, iScale, iOrig, iGround };
        for (GLint a : attrs)
            if (a >= 0) { VertexAttribDivisor(a, 0); DisableVertexAttribArray(a); }
        BindBuffer(GL_ARRAY_BUFFER, 0);
        UseProgram(0);

        // Restore through the cached helpers (mirror legacy RenderBodyShadow's end-state:
        // depth write on, stencil off) + EnableAlphaTest() = the opaque end-state InstFlush
        // guaranteed, so downstream effects/legacy draws see a consistent cached state.
        glDisable(GL_STENCIL_TEST);
        EnableDepthMask();
        EnableAlphaTest();
    }

    int  ShadowDrawCount()     { return s_drawCount; }
    int  ShadowInstanceCount() { return s_instCount; }

    void DropShadowBuffers()
    {
        s_drawBuckets.clear();
        for (auto& wm : s_workerBuckets)
            wm.clear();
    }

    void DropShadowBucketsFor(const BMD* model)
    {
        for (auto it = s_drawBuckets.begin(); it != s_drawBuckets.end(); )
        {
            if (it->second.model == model)
                it = s_drawBuckets.erase(it);
            else
                ++it;
        }
        for (auto& wm : s_workerBuckets)
            for (auto it = wm.begin(); it != wm.end(); )
            {
                if (it->second.model == model)
                    it = wm.erase(it);
                else
                    ++it;
            }
    }
}
