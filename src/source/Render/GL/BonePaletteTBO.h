#pragma once

#include <gl/glew.h>
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

        void Begin() { m_staging.clear(); }

        // Append a character's palette; returns the base BONE index to put in the
        // per-instance attribute. boneMatrix = BoneTransform-style [bone][3][4].
        int  AppendPalette(const float (*boneMatrix)[3][4], int boneCount);

        int  BoneCount() const { return (int)(m_staging.size() / kFloatsPerBone); }

        void Upload();                 // push staging -> GPU buffer (grows as needed)
        void Bind(int unit) const;     // activate `unit`, bind the buffer texture

    private:
        void Destroy();
        GLuint             m_buf = 0;
        GLuint             m_tex = 0;
        std::vector<float> m_staging;
        GLsizeiptr         m_capacityBytes = 0;
    };

    // Process-wide instance (one palette per frame, shared by all instanced draws).
    BonePaletteTBO& GetBonePaletteTBO();
}
