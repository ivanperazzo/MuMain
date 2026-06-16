#pragma once

#include <gl/glew.h>
#include <atomic>
#include <vector>

namespace Render::GL
{
    // Per-frame bone-matrix palette for instanced character skinning
    // (P-bmd-instance). Every visible character's bone matrices are appended to one
    // big buffer, uploaded once, and exposed as a texture buffer (samplerBuffer) so
    // the instanced vertex shader can fetch any character's bones by index. Each
    // bone is 3 texels (RGBA32F = the 3 rows of the legacy 3x4 affine, row-major).
    //
    // Frame flow: Begin() -> AppendPalette() per character (returns its base bone
    // index) -> Upload() -> Bind(unit). Requires LoadModernGL() (TexBuffer).
    class BonePaletteTBO
    {
    public:
        static constexpr int kFloatsPerBone = 12;   // 3 texels * 4

        ~BonePaletteTBO();

        bool Ensure();                 // create buffer + buffer-texture once
        bool Valid() const { return m_tex != 0; }

        // Begin a frame. `maxBoneFloats` pre-sizes the staging buffer to a generous
        // worst-case so AppendPalette can claim ranges lock-free (atomic cursor +
        // memcpy into a vector whose .data() never moves during the frame). The
        // vector is grown (never shrunk) once; subsequent frames reuse the capacity.
        // Etapa 3b 6.9: lock-free so the parallel Phase-B collect never races.
        void Begin(size_t maxBoneFloats = 0);

        // Append a character's palette; returns the base BONE index to put in the
        // per-instance attribute. boneMatrix = BoneTransform-style [bone][3][4].
        // Lock-free: reserves [off,off+n) via an atomic fetch_add, then writes the
        // bone floats directly into the pre-sized buffer. Globally-unique base index
        // (Option A) so paletteBase needs no rebase across workers.
        int  AppendPalette(const float (*boneMatrix)[3][4], int boneCount);

        // Floats actually claimed this frame (atomic cursor, NOT vector size).
        int  BoneCount() const { return (int)(m_cursor.load(std::memory_order_relaxed) / kFloatsPerBone); }

        void Upload();                 // push staging -> GPU buffer (grows as needed)
        void Bind(int unit) const;     // activate `unit`, bind the buffer texture

    private:
        void Destroy();
        GLuint               m_buf = 0;
        GLuint               m_tex = 0;
        std::vector<float>   m_staging;        // pre-sized at Begin(); .data() stable for the frame
        std::atomic<size_t>  m_cursor{0};      // floats claimed this frame (lock-free reserve)
        GLsizeiptr           m_capacityBytes = 0;
    };

    // Process-wide instance (one palette per frame, shared by all instanced draws).
    BonePaletteTBO& GetBonePaletteTBO();
}
