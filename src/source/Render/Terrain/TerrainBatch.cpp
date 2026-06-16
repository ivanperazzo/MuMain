#include "stdafx.h"

#include "Render/Terrain/TerrainBatch.h"
#include "Render/Textures/ZzzOpenglUtil.h"

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
        };

        // key = (texture << 4) | mode  -> stable element refs across rehash, so
        // TerrainBatchSelect can hand the caller a &data pointer to append into.
        std::unordered_map<uint64_t, Bucket> s_buckets;

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

    void TerrainBatchBegin()
    {
        for (auto& kv : s_buckets)
            kv.second.data.clear();   // keep capacity, reuse across frames
    }

    std::vector<float>* TerrainBatchSelect(int glTexture, int mode)
    {
        Bucket& b = s_buckets[Key(glTexture, mode)];
        b.glTexture = glTexture;
        b.mode = mode;
        b.data.reserve(b.data.size() + kFloatsPerQuad);
        return &b.data;
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
}
