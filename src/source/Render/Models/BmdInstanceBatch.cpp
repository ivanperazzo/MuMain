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

#include <gl/glew.h>
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

        struct Bucket
        {
            const BMD* model = nullptr;
            int        meshIndex = 0;
            int        texId = 0;
            int        mode = 0;           // 0 = textured, 1 = chrome (sphere-map)
            int        blend = 0;          // 0 = opaque (alpha-test), 1 = additive (GL_ONE/ONE)
            std::vector<float>     recs;   // flattened InstanceRec (10 floats each)
            Render::GL::GpuBuffer  instVbo;
        };

        std::unordered_map<uint64_t, Bucket> s_buckets;
        int s_drawCount = 0;
        int s_instCount = 0;
        float s_instLight[3] = { 0.f, 0.f, 0.f };   // global lit light dir (lit instances)
        float s_instWave = 0.f;                     // global chrome reflection scroll
        float s_chromeWave2 = 0.f;                  // CHROME2/6 scroll (frame-global)
        float s_chromeL[3] = { 0.f, 0.f, 1.f };     // CHROME4 light vec
        float s_chromeLightVec[3] = { 0.f, 0.f, 1.f }; // CHROME3 LightVector

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

    void InstBegin()
    {
        for (auto& kv : s_buckets)
            kv.second.recs.clear();
        Render::GL::GetBonePaletteTBO().Begin();
        s_drawCount = 0;
        s_instCount = 0;
        s_instLight[0] = s_instLight[1] = s_instLight[2] = 0.f;
        s_instWave = 0.f;
        s_chromeWave2 = 0.f;
        s_chromeL[0] = s_chromeL[1] = 0.f; s_chromeL[2] = 1.f;
        s_chromeLightVec[0] = s_chromeLightVec[1] = 0.f; s_chromeLightVec[2] = 1.f;
    }

    int InstAppendPalette(const float (*boneMatrix)[3][4], int boneCount)
    {
        return Render::GL::GetBonePaletteTBO().AppendPalette(boneMatrix, boneCount);
    }

    void InstSetLight(const float lightPos[3])
    {
        s_instLight[0] = lightPos[0]; s_instLight[1] = lightPos[1]; s_instLight[2] = lightPos[2];
    }

    void InstSetWave(float wave)
    {
        s_instWave = wave;
    }

    void InstSetChromeParams(float wave2, const float L[3], const float lightVec[3])
    {
        s_chromeWave2 = wave2;
        s_chromeL[0] = L[0]; s_chromeL[1] = L[1]; s_chromeL[2] = L[2];
        s_chromeLightVec[0] = lightVec[0]; s_chromeLightVec[1] = lightVec[1]; s_chromeLightVec[2] = lightVec[2];
    }

    void InstAdd(const BMD* model, int meshIndex, int texId, const InstanceRec& rec, int mode, int blend)
    {
        Bucket& b = s_buckets[Key(model, meshIndex, texId, mode, blend)];
        b.model = model; b.meshIndex = meshIndex; b.texId = texId; b.mode = mode; b.blend = blend;
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

        tbo.Upload();

        // Clear any GL error the legacy immediate-mode passes left this frame, so the
        // post-draw check below attributes errors to the instanced path only.
        while (glGetError() != GL_NO_ERROR) {}

        sh.Use();
        tbo.Bind(1);
        sh.SetPaletteUnit(1);
        sh.SetTextureUnit(0);
        sh.SetLight(s_instLight);   // global lit dir set during collect (lit instances)
        sh.SetChromeParams(s_chromeWave2, s_chromeL, s_chromeLightVec);   // chrome variants
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
            sh.SetWave(s_instWave);
            BindTexture(b.texId);
            DrawArraysInstanced(GL_TRIANGLES, 0, g->vertexCount, instances);
            ++s_drawCount;
            s_instCount += instances;
        };

        glEnable(GL_DEPTH_TEST);

        // Pass 1: opaque meshes (textured bodies + opaque chrome) — alpha-test, depth write.
        EnableAlphaTest();
        for (auto& kv : s_buckets)
            if (kv.second.blend == 0) drawBucket(kv.second);

        // Pass 2: additive chrome (RENDER_BRIGHT -> GL_ONE/GL_ONE; EnableAlphaBlend turns OFF
        // depth writes). Additive is order-independent, so no per-instance sort is needed; the
        // depth test still rejects chrome behind the opaque geometry drawn in pass 1.
        EnableAlphaBlend();
        for (auto& kv : s_buckets)
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

    void DropInstanceBuffers() { s_buckets.clear(); }

    void DropInstanceBucketsFor(const BMD* model)
    {
        for (auto it = s_buckets.begin(); it != s_buckets.end(); )
        {
            if (it->second.model == model)
                it = s_buckets.erase(it);   // Bucket dtor frees its instVbo
            else
                ++it;
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
