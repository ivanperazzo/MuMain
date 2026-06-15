#include "stdafx.h"

#include "Render/Models/BmdInstanceBatch.h"

#include "Render/Models/BmdGpuCache.h"
#include "Render/Models/ZzzBMD.h"
#include "Render/GL/BonePaletteTBO.h"
#include "Render/GL/BmdShader.h"
#include "Render/GL/InstancedBmdShader.h"
#include "Render/GL/GpuBuffer.h"
#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Render/Textures/ZzzTexture.h"
#include "Core/Jobs/ThreadPool.h"

#include <gl/glew.h>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Render::Models
{
    namespace
    {
        // Per-instance VBO layout (must match InstancedBmdShader attribs).
        // 10 floats: paletteBase(1) bodyScale(1) bodyOrigin(3) color(4) lit(1).
        constexpr GLsizei kInstFloats = 10;
        constexpr GLsizei kInstStride = kInstFloats * (GLsizei)sizeof(float);
        constexpr GLsizei kOffBase    = 0;
        constexpr GLsizei kOffScale   = 1 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffOrigin  = 2 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffColor   = 5 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffLit     = 9 * (GLsizei)sizeof(float);

        // GL-backed draw bucket: the GL-free payload (model/mesh/tex/mode/blend/uvScroll +
        // recs, defined in the header for the unit-testable merge) plus a per-bucket VBO.
        struct Bucket : InstBucketData
        {
            Render::GL::GpuBuffer  instVbo;
        };

        // Per-worker collection: each worker appends into its OWN map (s_workerBuckets[w]),
        // so the (eventually parallel) build never mutates shared state. Sized to the pool's
        // worker count; the serial path uses only worker 0 -> byte-identical output.
        // s_drawBuckets is the merged draw set built at flush.
        std::vector<InstBucketMap>           s_workerBuckets;
        std::unordered_map<uint64_t, Bucket> s_drawBuckets;
        int s_drawCount = 0;
        int s_instCount = 0;
        // sub-task 6.7: the chrome/wave/light shader inputs that used to be shared frame
        // globals (s_instLight/s_instWave/s_chromeWave2/s_chromeL/s_chromeLightVec) now ride
        // per-bucket (InstBucketData::instLight/instWave/chrome*), set at InstAdd and applied
        // per-bucket at flush. Per-worker buckets => a parallel collect never races them.

        // mode/blend in the top nibble (bits 60-63): on the x86 build a 32-bit model
        // pointer <<24 reaches bit 56 at most, so chrome/additive variants never collide
        // with the textured-opaque bucket of the same (model, mesh, tex).
        uint64_t Key(const BMD* m, int mesh, int tex, int mode, int blend)
        {
            return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)) << 24)
                 ^ (static_cast<uint64_t>(mode & 0x3) << 60)
                 ^ (static_cast<uint64_t>(blend & 0x3) << 62)
                 ^ (static_cast<uint64_t>(mesh & 0xFF) << 16)
                 ^ static_cast<uint64_t>(tex & 0xFFFF);
        }
    }

    namespace
    {
        int WorkerBucketCount()
        {
            const int n = Core::Jobs::ThreadPool::Instance().WorkerCount();
            return n < 1 ? 1 : n;
        }
    }

    namespace
    {
        // Etapa 3b 6.9: the bone-palette TBO must be PRE-SIZED before the (parallel)
        // collect so AppendPalette can claim ranges lock-free into a buffer whose
        // .data() never moves. We grow the worst-case to last frame's actual usage +
        // 50% headroom (and never shrink), with a floor that comfortably covers the
        // 200-char stress (~200 chars * ~16 parts * 96 bones * 12 floats ~= 3.7M).
        // The grow happens HERE, serially in Begin, never during ParallelFor.
        size_t s_paletteWorstFloats = 0;
        constexpr size_t kPaletteFloorFloats = 6u * 1024u * 1024u;   // ~24 MB, covers 200ch + slack
    }

    void InstBegin()
    {
        // Size the per-worker collection once; clear each worker's records (retain map
        // capacity so warm frames don't reallocate the buckets).
        const int workers = WorkerBucketCount();
        if ((int)s_workerBuckets.size() < workers)
            s_workerBuckets.resize(workers);
        for (auto& wm : s_workerBuckets)
            for (auto& kv : wm)
                kv.second.recs.clear();

        // Grow the palette worst-case from last frame's claimed floats (+ headroom).
        auto& tbo = Render::GL::GetBonePaletteTBO();
        const size_t lastUsed = (size_t)tbo.BoneCount() * Render::GL::BonePaletteTBO::kFloatsPerBone;
        size_t want = lastUsed + lastUsed / 2;          // +50% headroom for growth
        if (want < kPaletteFloorFloats) want = kPaletteFloorFloats;
        if (want > s_paletteWorstFloats) s_paletteWorstFloats = want;
        tbo.Begin(s_paletteWorstFloats);

        s_drawCount = 0;
        s_instCount = 0;
    }

    int InstAppendPalette(const float (*boneMatrix)[3][4], int boneCount)
    {
        return Render::GL::GetBonePaletteTBO().AppendPalette(boneMatrix, boneCount);
    }

    void InstAdd(const BMD* model, int meshIndex, int texId, const InstanceRec& rec, int mode, int blend)
    {
        // Append to the CALLING worker's bucket map (no shared mutation). On the serial path
        // CurrentWorkerIndex() == 0, so this is the single-map behaviour of pre-Task-4.
        const int w = Core::Jobs::ThreadPool::CurrentWorkerIndex();
        InstBucketMap& wm = s_workerBuckets[(w >= 0 && w < (int)s_workerBuckets.size()) ? w : 0];
        InstBucketData& b = wm[Key(model, meshIndex, texId, mode, blend)];
        b.model = model; b.meshIndex = meshIndex; b.texId = texId; b.mode = mode; b.blend = blend;
        b.uvScroll[0] = rec.uvScroll[0]; b.uvScroll[1] = rec.uvScroll[1];   // per-bucket UV offset (wave); identical across instances of this model+mesh
        // Per-bucket frame-global shader inputs (sub-task 6.7), formerly the shared s_inst*/
        // s_chrome* globals. Frame-constant across this bucket's instances; the last writer
        // (== identical value) wins, mirroring the old global-set semantics exactly.
        b.instLight[0] = rec.instLight[0]; b.instLight[1] = rec.instLight[1]; b.instLight[2] = rec.instLight[2];
        b.instWave = rec.instWave;
        b.chromeWave2 = rec.chromeWave2;
        b.chromeL[0] = rec.chromeL[0]; b.chromeL[1] = rec.chromeL[1]; b.chromeL[2] = rec.chromeL[2];
        b.chromeLightVec[0] = rec.chromeLightVec[0]; b.chromeLightVec[1] = rec.chromeLightVec[1]; b.chromeLightVec[2] = rec.chromeLightVec[2];
        b.recs.push_back(rec.paletteBase);
        b.recs.push_back(rec.bodyScale);
        b.recs.push_back(rec.bodyOrigin[0]);
        b.recs.push_back(rec.bodyOrigin[1]);
        b.recs.push_back(rec.bodyOrigin[2]);
        b.recs.push_back(rec.color[0]);
        b.recs.push_back(rec.color[1]);
        b.recs.push_back(rec.color[2]);
        b.recs.push_back(rec.color[3]);
        b.recs.push_back(rec.lit);
    }

    void InstFlush()
    {
        using namespace Render::GL;
        InstancedBmdShader& sh = GetInstancedBmdShader();
        BonePaletteTBO&     tbo = GetBonePaletteTBO();
        if (!sh.Ensure() || !tbo.Ensure())
            return;

        // Merge the per-worker collections into the draw set (order-independent). Retain
        // s_drawBuckets's GpuBuffers across frames (keyed VBOs reused); clear only records.
        for (auto& kv : s_drawBuckets)
            kv.second.recs.clear();
        {
            InstBucketMap merged;
            MergeBuckets(s_workerBuckets.data(), (int)s_workerBuckets.size(), merged);
            for (auto& kv : merged)
            {
                Bucket& dst = s_drawBuckets[kv.first];   // reuses existing instVbo if present
                static_cast<InstBucketData&>(dst) = std::move(kv.second);
            }
        }

        tbo.Upload();

        // Clear any GL error the legacy immediate-mode passes left this frame, so the
        // post-draw check below attributes errors to the instanced path only.
        while (glGetError() != GL_NO_ERROR) {}

        sh.Use();
        tbo.Bind(1);
        sh.SetPaletteUnit(1);
        sh.SetTextureUnit(0);
        // sub-task 6.7: SetLight/SetWave/SetChromeParams are now applied PER-BUCKET inside
        // drawBucket (the values ride in InstBucketData), not once for the whole flush.
        ActiveTexture(GL_TEXTURE0);

        const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone(), aN = sh.AttrNormal(),
                    aNB = sh.AttrNBone(), aUV = sh.AttrUV();
        const GLint iBase = sh.AttrPaletteBase(), iScale = sh.AttrBodyScale(),
                    iOrig = sh.AttrBodyOrigin(), iCol = sh.AttrColor(), iLit = sh.AttrLit();

        // Issue one bucket: bind shared geometry (divisor 0) + per-instance data
        // (divisor 1), set chrome/wave uniforms, bind texture, one instanced draw.
        auto drawBucket = [&](Bucket& b)
        {
            const int instances = (int)(b.recs.size() / kInstFloats);
            if (instances == 0)
                return;

            const MeshGpu* g = GetOrBuildMeshGpu(b.model, b.meshIndex, BmdShader::kMaxBones);
            if (g == nullptr || !g->eligible)
                return;

            // Per-vertex geometry (divisor 0).
            namespace L = Render::Models::GpuVtxLayout;
            g->vbo.Bind(GL_ARRAY_BUFFER);
            if (aPos >= 0) { EnableVertexAttribArray(aPos); VertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffPos);    VertexAttribDivisor(aPos, 0); }
            if (aVB  >= 0) { EnableVertexAttribArray(aVB);  VertexAttribPointer(aVB,  1, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffVBone);  VertexAttribDivisor(aVB, 0); }
            if (aN   >= 0) { EnableVertexAttribArray(aN);   VertexAttribPointer(aN,   3, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffNormal); VertexAttribDivisor(aN, 0); }
            if (aNB  >= 0) { EnableVertexAttribArray(aNB);  VertexAttribPointer(aNB,  1, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffNBone);  VertexAttribDivisor(aNB, 0); }
            if (aUV  >= 0) { EnableVertexAttribArray(aUV);  VertexAttribPointer(aUV,  2, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffUV);     VertexAttribDivisor(aUV, 0); }

            // Per-instance data (divisor 1).
            b.instVbo.Upload(GL_ARRAY_BUFFER, b.recs.data(),
                (GLsizeiptr)(b.recs.size() * sizeof(float)), GL_DYNAMIC_DRAW);
            b.instVbo.Bind(GL_ARRAY_BUFFER);
            if (iBase  >= 0) { EnableVertexAttribArray(iBase);  VertexAttribPointer(iBase,  1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffBase);   VertexAttribDivisor(iBase, 1); }
            if (iScale >= 0) { EnableVertexAttribArray(iScale); VertexAttribPointer(iScale, 1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffScale);  VertexAttribDivisor(iScale, 1); }
            if (iOrig  >= 0) { EnableVertexAttribArray(iOrig);  VertexAttribPointer(iOrig,  3, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffOrigin); VertexAttribDivisor(iOrig, 1); }
            if (iCol   >= 0) { EnableVertexAttribArray(iCol);   VertexAttribPointer(iCol,   4, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffColor);  VertexAttribDivisor(iCol, 1); }
            if (iLit   >= 0) { EnableVertexAttribArray(iLit);   VertexAttribPointer(iLit,   1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffLit);    VertexAttribDivisor(iLit, 1); }

            sh.SetChromeMode(b.mode);   // 0 textured / 1 chrome sphere-map
            // Per-bucket frame-global shader inputs (sub-task 6.7): formerly set once for the
            // whole flush from the shared s_inst*/s_chrome* globals. Frame-constant per bucket,
            // so setting them per-bucket yields byte-identical uniforms to the old path.
            sh.SetLight(b.instLight);
            sh.SetWave(b.instWave);
            sh.SetChromeParams(b.chromeWave2, b.chromeL, b.chromeLightVec);
            sh.SetUvScroll(b.uvScroll); // textured UV offset (wave); 0 for non-wave buckets
            BindTexture(b.texId);
            DrawArraysInstanced(GL_TRIANGLES, 0, g->vertexCount, instances);
            ++s_drawCount;
            s_instCount += instances;
        };

        glEnable(GL_DEPTH_TEST);

        // Pass 1: opaque meshes (textured bodies + opaque chrome) — alpha-test, depth write.
        EnableAlphaTest();
        for (auto& kv : s_drawBuckets)
            if (kv.second.blend == 0) drawBucket(kv.second);

        // Pass 2: additive chrome (RENDER_BRIGHT -> GL_ONE/GL_ONE; EnableAlphaBlend turns OFF
        // depth writes). Additive is order-independent, so no per-instance sort is needed; the
        // depth test still rejects chrome behind the opaque geometry drawn in pass 1.
        EnableAlphaBlend();
        for (auto& kv : s_drawBuckets)
            if (kv.second.blend == 1) drawBucket(kv.second);

        // Restore the opaque alpha-test end-state (depth write on) the textured-only flush
        // guaranteed, so downstream legacy draws see the same state as before.
        EnableAlphaTest();

        if (s_drawCount > 0)
        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                Render::GL::Log("[bmd_inst] GL error 0x%x after %d instanced draws", (unsigned)err, s_drawCount);
        }

        // Restore: disable + reset divisors so legacy gl*Pointer/immediate draws and
        // other shaders aren't disturbed.
        const GLint attrs[] = { aPos, aVB, aN, aNB, aUV, iBase, iScale, iOrig, iCol, iLit };
        for (GLint a : attrs)
        {
            if (a >= 0) { VertexAttribDivisor(a, 0); DisableVertexAttribArray(a); }
        }
        BindBuffer(GL_ARRAY_BUFFER, 0);
        UseProgram(0);
    }

    int  InstDrawCount()     { return s_drawCount; }
    int  InstInstanceCount() { return s_instCount; }

    void DropInstanceBuffers()
    {
        s_drawBuckets.clear();   // frees each Bucket's instVbo
        for (auto& wm : s_workerBuckets)
            wm.clear();
    }

    void DropInstanceBucketsFor(const BMD* model)
    {
        for (auto it = s_drawBuckets.begin(); it != s_drawBuckets.end(); )
        {
            if (it->second.model == model)
                it = s_drawBuckets.erase(it);   // Bucket dtor frees its instVbo
            else
                ++it;
        }
        for (auto& wm : s_workerBuckets)
        {
            for (auto it = wm.begin(); it != wm.end(); )
            {
                if (it->second.model == model)
                    it = wm.erase(it);
                else
                    ++it;
            }
        }
    }

    // Standalone runtime validation of the instanced draw pipeline (TBO texelFetch +
    // per-instance divisor attribs + glDrawArraysInstanced) on a throwaway triangle.
    // The login town has no characters, so without this the autonomous smoke only
    // proves the shader COMPILES, not that it DRAWS. Env-gated (MU_GPUINST_SELFTEST=1);
    // logs glGetError once. No-op otherwise.
    void InstSelfTest()
    {
        using namespace Render::GL;
        static const bool s_on = [] {
            char buf[8] = {}; size_t n = 0;
            return getenv_s(&n, buf, sizeof(buf), "MU_GPUINST_SELFTEST") == 0 && n > 0 && buf[0] == '1';
        }();
        if (!s_on)
            return;
        static bool s_done = false;
        if (s_done)
            return;

        InstancedBmdShader& sh = GetInstancedBmdShader();
        BonePaletteTBO&     tbo = GetBonePaletteTBO();
        if (!IsLoaded() || !sh.Ensure() || !tbo.Ensure())
            return;   // retry next frame until GL is up
        s_done = true;

        // 1 identity bone -> palette base 0.
        const float ident[1][3][4] = { { {1,0,0,0}, {0,1,0,0}, {0,0,1,0} } };
        tbo.Begin();
        const int base = tbo.AppendPalette(ident, 1);
        tbo.Upload();

        // Throwaway triangle in GpuVtxLayout (10 floats/vtx), all bone 0.
        namespace L = Render::Models::GpuVtxLayout;
        static GpuBuffer s_geo;
        if (!s_geo.Valid())
        {
            const float tri[3 * L::kFloats] = {
                  0.f,  0.f, 0.f,  0.f,  0.f,0.f,1.f,  0.f,  0.f,0.f,
                 50.f,  0.f, 0.f,  0.f,  0.f,0.f,1.f,  0.f,  1.f,0.f,
                  0.f, 50.f, 0.f,  0.f,  0.f,0.f,1.f,  0.f,  0.f,1.f,
            };
            s_geo.Upload(GL_ARRAY_BUFFER, tri, sizeof(tri), GL_STATIC_DRAW);
        }

        // 3 instances at staggered origins.
        const int N = 3;
        float inst[N * kInstFloats];
        for (int i = 0; i < N; ++i)
        {
            float* r = &inst[i * kInstFloats];
            r[0] = (float)base; r[1] = 1.f;                 // paletteBase, bodyScale
            r[2] = (float)(i * 60); r[3] = 0.f; r[4] = 0.f; // bodyOrigin
            r[5] = 1.f; r[6] = 1.f; r[7] = 1.f; r[8] = 1.f; // color
            r[9] = 0.f;                                     // lit
        }
        static GpuBuffer s_inst;
        s_inst.Upload(GL_ARRAY_BUFFER, inst, sizeof(inst), GL_DYNAMIC_DRAW);

        while (glGetError() != GL_NO_ERROR) {}   // clear pending errors

        sh.Use();
        tbo.Bind(1); sh.SetPaletteUnit(1); sh.SetTextureUnit(0);
        const float lp[3] = { 0.f, 0.f, 0.f };
        sh.SetLight(lp);
        ActiveTexture(GL_TEXTURE0);

        const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone(), aN = sh.AttrNormal(),
                    aNB = sh.AttrNBone(), aUV = sh.AttrUV();
        const GLint iBase = sh.AttrPaletteBase(), iScale = sh.AttrBodyScale(),
                    iOrig = sh.AttrBodyOrigin(), iCol = sh.AttrColor(), iLit = sh.AttrLit();

        s_geo.Bind(GL_ARRAY_BUFFER);
        if (aPos >= 0) { EnableVertexAttribArray(aPos); VertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffPos);    VertexAttribDivisor(aPos, 0); }
        if (aVB  >= 0) { EnableVertexAttribArray(aVB);  VertexAttribPointer(aVB,  1, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffVBone);  VertexAttribDivisor(aVB, 0); }
        if (aN   >= 0) { EnableVertexAttribArray(aN);   VertexAttribPointer(aN,   3, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffNormal); VertexAttribDivisor(aN, 0); }
        if (aNB  >= 0) { EnableVertexAttribArray(aNB);  VertexAttribPointer(aNB,  1, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffNBone);  VertexAttribDivisor(aNB, 0); }
        if (aUV  >= 0) { EnableVertexAttribArray(aUV);  VertexAttribPointer(aUV,  2, GL_FLOAT, GL_FALSE, L::kStride, (const GLvoid*)(size_t)L::kOffUV);     VertexAttribDivisor(aUV, 0); }

        s_inst.Bind(GL_ARRAY_BUFFER);
        if (iBase  >= 0) { EnableVertexAttribArray(iBase);  VertexAttribPointer(iBase,  1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffBase);   VertexAttribDivisor(iBase, 1); }
        if (iScale >= 0) { EnableVertexAttribArray(iScale); VertexAttribPointer(iScale, 1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffScale);  VertexAttribDivisor(iScale, 1); }
        if (iOrig  >= 0) { EnableVertexAttribArray(iOrig);  VertexAttribPointer(iOrig,  3, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffOrigin); VertexAttribDivisor(iOrig, 1); }
        if (iCol   >= 0) { EnableVertexAttribArray(iCol);   VertexAttribPointer(iCol,   4, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffColor);  VertexAttribDivisor(iCol, 1); }
        if (iLit   >= 0) { EnableVertexAttribArray(iLit);   VertexAttribPointer(iLit,   1, GL_FLOAT, GL_FALSE, kInstStride, (const GLvoid*)(size_t)kOffLit);    VertexAttribDivisor(iLit, 1); }

        DrawArraysInstanced(GL_TRIANGLES, 0, 3, N);
        const GLenum err = glGetError();
        Render::GL::Log("[bmd_inst] selftest: DrawArraysInstanced(3 verts x %d inst) base=%d -> GL err=0x%x (%s)",
            N, base, (unsigned)err, err == GL_NO_ERROR ? "OK" : "FAIL");

        const GLint attrs[] = { aPos, aVB, aN, aNB, aUV, iBase, iScale, iOrig, iCol, iLit };
        for (GLint a : attrs)
            if (a >= 0) { VertexAttribDivisor(a, 0); DisableVertexAttribArray(a); }
        BindBuffer(GL_ARRAY_BUFFER, 0);
        UseProgram(0);
    }
}
