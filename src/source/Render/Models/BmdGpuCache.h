#pragma once

#include "Render/GL/GpuBuffer.h"

#include <gl/glew.h>

class BMD;

namespace Render::Models
{
    // Per-mesh GPU geometry, built once from a BMD mesh and kept resident. Model-
    // space positions/normals + bone indices + UVs (skinning happens in the shader,
    // so this never changes per frame). Built lazily on first draw; keyed by
    // (BMD*, meshIndex).
    struct MeshGpu
    {
        Render::GL::GpuBuffer vbo;
        GLsizei vertexCount = 0;     // = NumTriangles*3 for triangle meshes
        bool    eligible    = false; // false -> caller must use the legacy path
        bool    built       = false;
    };

    // Interleaved vertex layout uploaded to the VBO (matches BmdShader attribs).
    // 10 floats: pos(3) vBone(1) normal(3) nBone(1) uv(2).
    namespace GpuVtxLayout
    {
        constexpr int   kFloats     = 10;
        constexpr GLsizei kStride    = kFloats * (GLsizei)sizeof(float);
        constexpr GLsizei kOffPos    = 0;
        constexpr GLsizei kOffVBone  = 3 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffNormal = 4 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffNBone  = 7 * (GLsizei)sizeof(float);
        constexpr GLsizei kOffUV     = 8 * (GLsizei)sizeof(float);
    }

    // Returns the cached MeshGpu for (model, meshIndex), building it on first call.
    // nullptr on bad args. Check ->eligible before drawing it on the GPU path.
    // maxBones gates eligibility (bone index must be < maxBones for the shader).
    const MeshGpu* GetOrBuildMeshGpu(const BMD* model, int meshIndex, int maxBones);

    // Drop all cached buffers (e.g. on map change / shutdown). Optional.
    void ClearGpuCache();

    // Runtime toggle (console "$gpubmd on/off", default off): master switch for the
    // BMD-to-GPU path. Off -> everything renders the legacy CPU way.
    void SetGpuBmdEnabled(bool on);
    bool GpuBmdEnabled();

    // Set true ONLY around the Objects (props) render pass so the GPU path stays
    // confined to props for now (characters keep the legacy path). MainScene flips
    // it around each RenderObjects() call.
    void SetGpuObjectsPass(bool on);
    bool GpuObjectsPass();
}
