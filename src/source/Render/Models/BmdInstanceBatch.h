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
    // mode:  0 = textured (sample aUV), 1 = chrome (sphere-map UV from normal + scroll).
    // blend: 0 = opaque (alpha-test, depth write), 1 = additive (RENDER_BRIGHT, GL_ONE/ONE,
    //        no depth write, order-independent). Each (mode, blend) is a separate bucket so
    //        FLUSH draws opaque first then additive, switching uChromeMode/uWave per bucket.
    void InstAdd(const BMD* model, int meshIndex, int texId, const InstanceRec& rec, int mode = 0, int blend = 0);
    void InstSetLight(const float lightPos[3]);   // global lit dir for lit instances
    void InstSetWave(float wave);                 // global chrome reflection scroll
    // CHROME2/3/4/6 extra frame-global inputs (Wave2 scroll, CHROME4 L, CHROME3 LightVector).
    void InstSetChromeParams(float wave2, const float L[3], const float lightVec[3]);
    void InstFlush();

    // Stats for the diagnostics log: instanced draws issued + instances covered.
    int  InstDrawCount();
    int  InstInstanceCount();

    // Runtime self-test (env MU_GPUINST_SELFTEST=1): draws a throwaway instanced
    // triangle once and logs glGetError, so the autonomous smoke (login town has no
    // characters) can still prove glDrawArraysInstanced works on this driver.
    void InstSelfTest();

    void DropInstanceBuffers();   // free GPU buffers (map change / shutdown)

    // Drop buckets referencing one model (its geometry was freed/reloaded in place).
    void DropInstanceBucketsFor(const BMD* model);
}
