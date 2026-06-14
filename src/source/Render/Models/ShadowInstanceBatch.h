#pragma once

class BMD;

namespace Render::Models
{
    // Instanced character SHADOW batching (P-bmd-shadow). During the Characters pass,
    // BMD::RenderBodyShadow COLLECTS each shadow-casting mesh (instead of the legacy
    // per-vertex CalcShadowPosition + immediate draw) into buckets keyed by
    // (BMD*, meshIndex); at the end of the pass FLUSH issues one glDrawArraysInstanced
    // per bucket. Reuses the same model geometry VBO (BmdGpuCache) and the same
    // BonePaletteTBO the body instancing already populated this frame.
    //
    // Frame flow: ShadowBegin() -> per shadow-casting mesh ShadowAdd() -> ShadowFlush().
    // ShadowFlush MUST run after InstFlush() (which uploads the palette TBO).

    // One per-instance record (must match the shadow instanced VBO layout / shader attribs).
    struct ShadowRec
    {
        float paletteBase;     // base bone index in the TBO (same as the body's)
        float bodyScale;
        float bodyOrigin[3];
        float groundZ;         // RequestTerrainHeight(origin) + 5, sampled once per char
    };

    void ShadowBegin();
    // sx/sy are the CalcShadowPosition skew constants (map-global: InBattleCastle?2500:2000,
    // 4000). Identical for every character in a frame; stored as the frame-global skew.
    void ShadowAdd(const BMD* model, int meshIndex, const ShadowRec& rec, float sx, float sy);
    void ShadowFlush();

    // Stats for the diagnostics log.
    int  ShadowDrawCount();
    int  ShadowInstanceCount();

    void DropShadowBuffers();                       // free GPU buffers (map change / shutdown)
    void DropShadowBucketsFor(const BMD* model);    // model geometry freed/reloaded in place

    // Master toggle (env MU_GPUSHADOW=1, default off until validated). When off,
    // RenderBodyShadow keeps the legacy CPU path.
    bool GpuShadowEnabled();
}
