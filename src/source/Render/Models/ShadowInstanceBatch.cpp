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

#include <gl/glew.h>
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

        struct Bucket
        {
            const BMD* model = nullptr;
            int        meshIndex = 0;
            std::vector<float>     recs;   // flattened ShadowRec (6 floats each)
            Render::GL::GpuBuffer  instVbo;
        };

        std::unordered_map<uint64_t, Bucket> s_buckets;
        int   s_drawCount = 0;
        int   s_instCount = 0;
        float s_sx = 2000.f;
        float s_sy = 4000.f;

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
        for (auto& kv : s_buckets)
            kv.second.recs.clear();
        s_drawCount = 0;
        s_instCount = 0;
    }

    void ShadowAdd(const BMD* model, int meshIndex, const ShadowRec& rec, float sx, float sy)
    {
        s_sx = sx;
        s_sy = sy;
        Bucket& b = s_buckets[Key(model, meshIndex)];
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

        // The body InstFlush already uploaded the palette this frame; re-bind it.
        // (Upload is idempotent; calling it here makes the pass order-independent in
        // case no body instances flushed.)
        tbo.Upload();

        while (glGetError() != GL_NO_ERROR) {}

        sh.Use();
        tbo.Bind(1);
        sh.SetPaletteUnit(1);
        sh.SetSkew(s_sx, s_sy);
        const float kShadowColor[4] = { 0.f, 0.f, 0.f, 0.5f };   // legacy: 50% black
        sh.SetColor(kShadowColor);

        const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone();
        const GLint iBase = sh.AttrPaletteBase(), iScale = sh.AttrBodyScale(),
                    iOrig = sh.AttrBodyOrigin(), iGround = sh.AttrGroundZ();

        // Replicate RenderBodyShadow's GL state: alpha test off, 50% alpha blend,
        // no depth write (depth test stays on so shadows are occluded by geometry in
        // front), stencil INCR/ALWAYS (matches the legacy default stencil func).
        EnableAlphaTest(false);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

        namespace L = Render::Models::GpuVtxLayout;
        for (auto& kv : s_buckets)
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

        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_TRUE);
        EnableAlphaTest();
    }

    int  ShadowDrawCount()     { return s_drawCount; }
    int  ShadowInstanceCount() { return s_instCount; }

    void DropShadowBuffers() { s_buckets.clear(); }

    void DropShadowBucketsFor(const BMD* model)
    {
        for (auto it = s_buckets.begin(); it != s_buckets.end(); )
        {
            if (it->second.model == model)
                it = s_buckets.erase(it);
            else
                ++it;
        }
    }
}
