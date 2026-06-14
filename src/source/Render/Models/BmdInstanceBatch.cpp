#include "stdafx.h"

#include "Render/Models/BmdInstanceBatch.h"

#include "Render/Models/BmdGpuCache.h"
#include "Render/Models/ZzzBMD.h"
#include "Render/GL/BonePaletteTBO.h"
#include "Render/GL/BmdShader.h"
#include "Render/GL/InstancedBmdShader.h"
#include "Render/GL/GpuBuffer.h"
#include "Render/GL/GLLoader.h"
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
            std::vector<float>     recs;   // flattened InstanceRec (10 floats each)
            Render::GL::GpuBuffer  instVbo;
        };

        std::unordered_map<uint64_t, Bucket> s_buckets;
        int s_drawCount = 0;
        int s_instCount = 0;

        uint64_t Key(const BMD* m, int mesh, int tex)
        {
            return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)) << 24)
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
    }

    int InstAppendPalette(const float (*boneMatrix)[3][4], int boneCount)
    {
        return Render::GL::GetBonePaletteTBO().AppendPalette(boneMatrix, boneCount);
    }

    void InstAdd(const BMD* model, int meshIndex, int texId, const InstanceRec& rec)
    {
        Bucket& b = s_buckets[Key(model, meshIndex, texId)];
        b.model = model; b.meshIndex = meshIndex; b.texId = texId;
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

    void InstFlush(const float lightPos[3])
    {
        using namespace Render::GL;
        InstancedBmdShader& sh = GetInstancedBmdShader();
        BonePaletteTBO&     tbo = GetBonePaletteTBO();
        if (!sh.Ensure() || !tbo.Ensure())
            return;

        tbo.Upload();

        sh.Use();
        tbo.Bind(1);
        sh.SetPaletteUnit(1);
        sh.SetTextureUnit(0);
        sh.SetLight(lightPos);
        ActiveTexture(GL_TEXTURE0);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        EnableAlphaTest();   // opaque + cutout (all instanced meshes are opaque)

        const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone(), aN = sh.AttrNormal(),
                    aNB = sh.AttrNBone(), aUV = sh.AttrUV();
        const GLint iBase = sh.AttrPaletteBase(), iScale = sh.AttrBodyScale(),
                    iOrig = sh.AttrBodyOrigin(), iCol = sh.AttrColor(), iLit = sh.AttrLit();

        for (auto& kv : s_buckets)
        {
            Bucket& b = kv.second;
            const int instances = (int)(b.recs.size() / kInstFloats);
            if (instances == 0)
                continue;

            const MeshGpu* g = GetOrBuildMeshGpu(b.model, b.meshIndex, BmdShader::kMaxBones);
            if (g == nullptr || !g->eligible)
                continue;

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

            BindTexture(b.texId);
            DrawArraysInstanced(GL_TRIANGLES, 0, g->vertexCount, instances);
            ++s_drawCount;
            s_instCount += instances;
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
}
