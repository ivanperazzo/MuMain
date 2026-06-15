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

    // Drop the cached GPU geometry for one model. MUST be called when a BMD slot is
    // reloaded in place (BMD::Release) — the slot keeps its address but its Meshs are
    // freed/replaced, so the old VBOs (keyed by BMD*) would be stale (wrong geometry).
    void InvalidateGpuModel(const BMD* model);

    // Runtime toggle (console "$gpubmd on/off", default off): master switch for the
    // BMD-to-GPU path. Off -> everything renders the legacy CPU way.
    void SetGpuBmdEnabled(bool on);
    bool GpuBmdEnabled();

    // Set true ONLY around the Objects (props) render pass so the GPU path stays
    // confined to props (MainScene flips it around each RenderObjects() call).
    void SetGpuObjectsPass(bool on);
    bool GpuObjectsPass();

    // Same, for the Characters pass (players/mobs + their parts). P-bmd-chars.
    // MainScene flips it around RenderCharactersClient().
    void SetGpuCharsPass(bool on);
    bool GpuCharsPass();

    // Runtime toggle ("$gpuinst on/off", env MU_GPUINST=1, default off): in the
    // Characters pass, COLLECT eligible flat meshes into the instanced batch
    // (BmdInstanceBatch) and flush once per pass instead of one draw per mesh.
    // Requires GpuBmdEnabled() too (shares the GPU geometry cache + shader infra).
    void SetGpuInstEnabled(bool on);
    bool GpuInstEnabled();

    // Runtime toggle ("$gpublendmesh on/off", env MU_GPUBLENDMESH, default ON):
    // route translucent blend meshes (item glows / wing membranes) through the
    // per-mesh GPU path instead of legacy CPU-skin + immediate draw. Off -> legacy.
    // Runtime-settable for the planned in-game antilag panel. (Etapa 1.3.)
    void SetGpuBlendMeshEnabled(bool on);
    bool GpuBlendMeshEnabled();

    // P-bmd-skinskip: production skip-skin (env MU_GPUSKIN=1, default off). When on AND
    // in the instanced Characters pass with GPU shadows on, BMD::Transform DEFERS the
    // per-vertex CPU skin; consumers that read VertexTransform/NormalTransform force-skin
    // lazily (BMD::EnsureMeshSkinned). Saves the skin cost of meshes that only render via
    // the instanced GPU path + GPU shadow (nothing reads their CPU skin).
    bool GpuSkinDeferEnabled();

    // --- Diagnostics (measurement only) ---
    // "$skinskip on": BMD::Transform skips its per-vertex CPU skinning loops. Breaks
    // visuals, but isolates how much of the Characters cost is CPU skinning.
    void SetSkinSkip(bool on);
    bool SkinSkip();

    // Count one Characters-pass mesh draw (wentGpu = took the GPU path). Logged
    // periodically by LogAndResetGpuStats() so we can see GPU vs legacy coverage.
    void NoteCharMeshDraw(bool wentGpu);
    // Coverage class for one char mesh: 0=instanced, 1=per-mesh GPU,
    // 2=legacy non-RENDER_TEXTURE (chrome/color), 3=legacy alpha-blend, 4=legacy wave,
    // 5=legacy bone/body scale, 6=legacy ineligible geometry, 7=legacy other.
    void NoteCharMeshClass(int cls);
    void NoteVisibleChar();       // one visible character rendered this frame
    void LogAndResetGpuStats();   // call once per frame
}
