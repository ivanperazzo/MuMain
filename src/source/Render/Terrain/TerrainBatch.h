#pragma once

#include <vector>

// P2 (terrain-VBO, Enfoque A+): batched fixed-function terrain rendering. The
// legacy path draws each terrain tile with its own glBegin/glEnd + BindTexture
// (thousands of immediate-mode calls/frame). Here the per-tile Vertex*() output
// (position/texcoord/colour, already computed each frame in ZzzLodTerrain
// globals) is accumulated into buckets keyed by (texture, blend-mode); at the
// end of each terrain pass one glDrawArrays(GL_QUADS) is issued per bucket,
// collapsing the draws/binds. No shader: one texture per bucket, per-vertex
// colour via the colour array, alpha in the A channel.
//
// Gated by env MU_TERRAINVBO (default off -> legacy path, zero behaviour change).
// Geometry/texcoords could be static, but per-vertex colour is dynamic
// (PrimaryTerrainLight changes every frame via AddTerrainLight), so the whole
// vertex is streamed each frame -- still far cheaper than immediate mode.

namespace Render::Terrain
{
    // Blend/state mode for a bucket; applied once before its draw at flush.
    enum TerrainBatchMode
    {
        TB_OPAQUE = 0,   // DisableAlphaBlend (base layer)
        TB_ALPHATEST,    // EnableAlphaTest   (alpha overlay / special-map base)
        TB_ALPHABLEND,   // EnableAlphaBlend  (water blend layer)
    };

    bool TerrainBatchEnabled();   // env MU_TERRAINVBO, read once

    size_t TerrainBatchVertexCount();   // TEMP: total verts buffered this pass (instrumentation)

    void TerrainBatchBegin();     // start a frame's terrain pass: reset buckets

    // Reserve one quad (4 verts x 9 floats: x y z u v r g b a) in the bucket for
    // (glTexture, mode) and return a raw cursor to its first float. The caller
    // writes exactly kFloatsPerQuad floats through the cursor (no per-vertex
    // push_back, no bounds checks). A last-key cache skips the hash lookup for
    // runs of same-texture tiles (terrain is spatially coherent). The returned
    // pointer is valid until the next TerrainBatchQuad call on the SAME bucket.
    float* TerrainBatchQuad(int glTexture, int mode);

    // Flush all buckets: opaque -> alphatest -> alphablend, one
    // glDrawArrays(GL_QUADS) each (BindTexture + state per bucket). Clears them.
    void TerrainBatchFlush();

    // Static-bake (MU_TERRAINSTATIC): after a full-map walk filled the buckets, upload
    // each to a GL_STATIC_DRAW VBO once. TerrainBatchDrawStatic then redraws straight
    // from the GPU VBOs every frame (no per-frame CPU->GPU vertex copy), making the
    // terrain normal pass view-independent. Geometry/colour are frozen at bake time.
    void TerrainBatchUploadStatic();
    void TerrainBatchDrawStatic();
}
