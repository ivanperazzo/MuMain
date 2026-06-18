#include "stdafx.h"

#include "Render/Terrain/TerrainBatch.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Render/GL/GLLoader.h"   // Render::GL::GenBuffers/BindBuffer/BufferData (GL 1.5)

#include <gl/glew.h>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

namespace Render::Terrain
{
    namespace
    {
        constexpr int kFloatsPerVert = 9;   // x y z u v r g b a
        constexpr int kFloatsPerQuad = kFloatsPerVert * 4;

        struct Bucket
        {
            int                glTexture = 0;
            int                mode = TB_OPAQUE;
            std::vector<float> data;        // interleaved, 9 floats/vertex
            GLuint             vbo = 0;      // static-bake GPU buffer (0 = none)
            GLsizei            vboVerts = 0; // verts uploaded to vbo (0 = skip in DrawStatic)
        };

        // key = (texture << 4) | mode  -> stable element refs across rehash, so
        // TerrainBatchQuad can hand the caller a &data pointer to append into.
        std::unordered_map<uint64_t, Bucket> s_buckets;

        // Last-resolved bucket cache: terrain walks tiles in spatial order, so long
        // runs share one texture+mode. unordered_map node refs are stable across
        // rehash, so a cached Bucket* stays valid for the whole pass.
        uint64_t s_lastKey    = ~0ull;
        Bucket*  s_lastBucket = nullptr;

        uint64_t Key(int glTexture, int mode)
        {
            return (static_cast<uint64_t>(glTexture) << 4) ^ static_cast<uint64_t>(mode & 0xF);
        }

        void ApplyMode(int mode)
        {
            switch (mode)
            {
            case TB_ALPHATEST:  EnableAlphaTest();    break;
            case TB_ALPHABLEND: EnableAlphaBlend();   break;
            default:            DisableAlphaBlend();  break;   // TB_OPAQUE
            }
        }

        void DrawBucketsOfMode(int mode)
        {
            for (auto& kv : s_buckets)
            {
                Bucket& b = kv.second;
                if (b.mode != mode || b.data.empty())
                    continue;

                ApplyMode(b.mode);
                BindTexture(b.glTexture);

                const float* base = b.data.data();
                const GLsizei stride = kFloatsPerVert * (GLsizei)sizeof(float);
                glVertexPointer(3, GL_FLOAT, stride, base + 0);
                glTexCoordPointer(2, GL_FLOAT, stride, base + 3);
                glColorPointer(4, GL_FLOAT, stride, base + 5);
                glDrawArrays(GL_QUADS, 0, (GLsizei)(b.data.size() / kFloatsPerVert));
            }
        }
    }

    bool TerrainBatchEnabled()
    {
        // Default ON (validated on login + in-game maps); opt out with MU_TERRAINVBO=0.
        static int s_enabled = -1;
        if (s_enabled < 0)
        {
            const char* e = std::getenv("MU_TERRAINVBO");
            s_enabled = (e && e[0] == '0') ? 0 : 1;
        }
        return s_enabled != 0;
    }

    size_t TerrainBatchVertexCount()
    {
        size_t floats = 0;
        for (auto& kv : s_buckets)
            floats += kv.second.data.size();
        return floats / kFloatsPerVert;
    }

    void TerrainBatchBegin()
    {
        s_lastKey = ~0ull;
        s_lastBucket = nullptr;
        for (auto& kv : s_buckets)
        {
            kv.second.data.clear();     // keep capacity, reuse across frames
            kv.second.vboVerts = 0;     // invalidate any prior static upload (re-bake)
        }
    }

    float* TerrainBatchQuad(int glTexture, int mode)
    {
        const uint64_t k = Key(glTexture, mode);
        Bucket* b;
        if (k == s_lastKey && s_lastBucket)
        {
            b = s_lastBucket;
        }
        else
        {
            b = &s_buckets[k];
            b->glTexture = glTexture;
            b->mode = mode;
            s_lastKey = k;
            s_lastBucket = b;
        }
        const size_t old = b->data.size();
        b->data.resize(old + kFloatsPerQuad);   // capacity reused across frames
        return b->data.data() + old;
    }

    void TerrainBatchFlush()
    {
        bool any = false;
        for (auto& kv : s_buckets)
            if (!kv.second.data.empty()) { any = true; break; }
        if (!any)
            return;

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);

        // Order matters: opaque base first (depth write), then alpha-tested
        // overlays, then alpha-blended water -- mirrors the per-tile legacy order
        // so layer-2 lands on its base. Tiles are coplanar/disjoint in XY, so the
        // order within a single mode is irrelevant.
        DrawBucketsOfMode(TB_OPAQUE);
        DrawBucketsOfMode(TB_ALPHATEST);
        DrawBucketsOfMode(TB_ALPHABLEND);

        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);

        // Restore a sane current colour (immediate-mode callers after us assume white).
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }

    void TerrainBatchUploadStatic()
    {
        for (auto& kv : s_buckets)
        {
            Bucket& b = kv.second;
            if (b.data.empty())
                continue;
            if (b.vbo == 0)
                Render::GL::GenBuffers(1, &b.vbo);
            Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.vbo);
            Render::GL::BufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(b.data.size() * sizeof(float)),
                         b.data.data(), GL_STATIC_DRAW);
            b.vboVerts = (GLsizei)(b.data.size() / kFloatsPerVert);
        }
        Render::GL::BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void TerrainBatchDrawStatic()
    {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);

        const GLsizei stride = kFloatsPerVert * (GLsizei)sizeof(float);
        for (int mode = TB_OPAQUE; mode <= TB_ALPHABLEND; ++mode)
        {
            for (auto& kv : s_buckets)
            {
                Bucket& b = kv.second;
                if (b.mode != mode || b.vbo == 0 || b.vboVerts == 0)
                    continue;
                ApplyMode(b.mode);
                BindTexture(b.glTexture);
                Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.vbo);
                // Offsets are byte offsets into the bound VBO (not client pointers).
                glVertexPointer(3, GL_FLOAT, stride, (const void*)(0));
                glTexCoordPointer(2, GL_FLOAT, stride, (const void*)(3 * sizeof(float)));
                glColorPointer(4, GL_FLOAT, stride, (const void*)(5 * sizeof(float)));
                glDrawArrays(GL_QUADS, 0, b.vboVerts);
            }
        }
        Render::GL::BindBuffer(GL_ARRAY_BUFFER, 0);

        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }
}
