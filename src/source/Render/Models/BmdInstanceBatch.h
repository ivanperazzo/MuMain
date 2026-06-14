#pragma once

class BMD;

namespace Render::Models
{
    // Instanced character batching (P-bmd-instance). During the Characters pass,
    // eligible meshes are COLLECTED (not drawn) into buckets keyed by
    // (BMD*, meshIndex, texture); at the end of the pass FLUSH issues one
    // glDrawArraysInstanced per bucket, collapsing N identical character meshes
    // into one draw. Bone matrices live in the BonePaletteTBO; per-instance data
    // (palette base, body scale/origin, colour, lit) in an instanced VBO.
    //
    // Frame flow: InstBegin() -> per char InstAppendPalette() -> per eligible mesh
    // InstAdd() -> InstFlush(lightPos).

    // One per-instance record (must match the instanced VBO layout / shader attribs).
    struct InstanceRec
    {
        float paletteBase;     // base bone index in the TBO
        float bodyScale;
        float bodyOrigin[3];
        float color[4];        // rgb base colour, a = alpha
        float lit;             // 1 = per-normal lighting, 0 = flat
    };

    void InstBegin();
    int  InstAppendPalette(const float (*boneMatrix)[3][4], int boneCount);
    void InstAdd(const BMD* model, int meshIndex, int texId, const InstanceRec& rec);
    void InstFlush(const float lightPos[3]);

    // Stats for the diagnostics log: instanced draws issued + instances covered.
    int  InstDrawCount();
    int  InstInstanceCount();

    void DropInstanceBuffers();   // free GPU buffers (map change / shutdown)
}
