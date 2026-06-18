#include "stdafx.h"

#include "Render/Terrain/TerrainBatch.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Render/GL/GLLoader.h"      // Render::GL::GenBuffers/BindBuffer/BufferData (GL 1.5)
#include "Render/Terrain/ZzzLodTerrain.h"  // PrimaryTerrainLight, TERRAIN_INDEX (colour streaming)

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
            std::vector<float> data;        // interleaved, 9 floats/vertex (per-frame walk)

            // Static-bake (MU_TERRAINSTATIC): geometry is uploaded once to a STATIC_DRAW
            // VBO (pos+uv, 5 floats/vert); per-vertex colour lives in a DYNAMIC_DRAW VBO
            // refreshed each frame from PrimaryTerrainLight so lighting stays live. vtxIdx
            // maps each vertex back to its terrain cell for the light lookup; colorScratch
            // is the CPU staging buffer (rgba; alpha baked once, rgb rewritten per frame).
            GLuint             posUvVbo = 0;
            GLuint             colorVbo = 0;
            GLsizei            vboVerts = 0;   // 0 = skip in DrawStatic
            std::vector<int>   vtxIdx;         // terrain cell index per vertex
            std::vector<float> colorScratch;   // rgba per vertex (4 floats/vert)
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

    namespace
    {
        constexpr int kPosUvFloats = 5;   // x y z u v (static)
        constexpr int kColorFloats = 4;   // r g b a (dynamic)
    }

    void TerrainBatchUploadStatic()
    {
        for (auto& kv : s_buckets)
        {
            Bucket& b = kv.second;
            if (b.data.empty())
                continue;

            const size_t verts = b.data.size() / kFloatsPerVert;

            // De-interleave the per-frame walk output into a static pos/uv stream and a
            // dynamic colour stream, and record each vertex's terrain cell (derived from
            // its XY) so the colour can be refreshed from PrimaryTerrainLight each frame.
            std::vector<float> posUv(verts * kPosUvFloats);
            b.colorScratch.assign(verts * kColorFloats, 1.f);
            b.vtxIdx.resize(verts);
            for (size_t v = 0; v < verts; ++v)
            {
                const float* src = &b.data[v * kFloatsPerVert];
                float* pu = &posUv[v * kPosUvFloats];
                pu[0] = src[0]; pu[1] = src[1]; pu[2] = src[2];   // pos
                pu[3] = src[3]; pu[4] = src[4];                   // uv
                float* col = &b.colorScratch[v * kColorFloats];
                col[0] = src[5]; col[1] = src[6]; col[2] = src[7]; col[3] = src[8];  // baked rgba

                int xi = (int)(src[0] / TERRAIN_SCALE + 0.5f);
                int yi = (int)(src[1] / TERRAIN_SCALE + 0.5f);
                if (xi < 0) xi = 0; else if (xi > TERRAIN_SIZE_MASK) xi = TERRAIN_SIZE_MASK;
                if (yi < 0) yi = 0; else if (yi > TERRAIN_SIZE_MASK) yi = TERRAIN_SIZE_MASK;
                b.vtxIdx[v] = TERRAIN_INDEX(xi, yi);
            }

            if (b.posUvVbo == 0) Render::GL::GenBuffers(1, &b.posUvVbo);
            Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.posUvVbo);
            Render::GL::BufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(posUv.size() * sizeof(float)),
                         posUv.data(), GL_STATIC_DRAW);

            if (b.colorVbo == 0) Render::GL::GenBuffers(1, &b.colorVbo);
            Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.colorVbo);
            Render::GL::BufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(b.colorScratch.size() * sizeof(float)),
                         b.colorScratch.data(), GL_DYNAMIC_DRAW);

            b.vboVerts = (GLsizei)verts;
        }
        Render::GL::BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void TerrainBatchUpdateColors()
    {
        // Refresh the dynamic colour VBO from the current PrimaryTerrainLight (alpha kept
        // from the bake). O(baked verts) flat loop -- no frustum/vertex math/GL per tile --
        // so the cost is constant regardless of camera, and lighting stays live.
        for (auto& kv : s_buckets)
        {
            Bucket& b = kv.second;
            if (b.colorVbo == 0 || b.vboVerts == 0)
                continue;
            const size_t verts = (size_t)b.vboVerts;
            for (size_t v = 0; v < verts; ++v)
            {
                const float* L = PrimaryTerrainLight[b.vtxIdx[v]];
                float* col = &b.colorScratch[v * kColorFloats];
                col[0] = L[0]; col[1] = L[1]; col[2] = L[2];   // rgb live; alpha untouched
            }
            Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.colorVbo);
            Render::GL::BufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(b.colorScratch.size() * sizeof(float)),
                         b.colorScratch.data(), GL_DYNAMIC_DRAW);
        }
        Render::GL::BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void TerrainBatchDrawStatic()
    {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);

        const GLsizei puStride = kPosUvFloats * (GLsizei)sizeof(float);
        const GLsizei cStride  = kColorFloats * (GLsizei)sizeof(float);
        for (int mode = TB_OPAQUE; mode <= TB_ALPHABLEND; ++mode)
        {
            for (auto& kv : s_buckets)
            {
                Bucket& b = kv.second;
                if (b.mode != mode || b.posUvVbo == 0 || b.vboVerts == 0)
                    continue;
                ApplyMode(b.mode);
                BindTexture(b.glTexture);
                // pos/uv from the static VBO; colour from the dynamic VBO. Each gl*Pointer
                // captures the buffer bound at its call, so binding flips between them.
                Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.posUvVbo);
                glVertexPointer(3, GL_FLOAT, puStride, (const void*)(0));
                glTexCoordPointer(2, GL_FLOAT, puStride, (const void*)(3 * sizeof(float)));
                Render::GL::BindBuffer(GL_ARRAY_BUFFER, b.colorVbo);
                glColorPointer(4, GL_FLOAT, cStride, (const void*)(0));
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
