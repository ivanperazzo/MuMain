#pragma once

#include "Render/GL/ShaderProgram.h"

#include <gl/glew.h>

namespace Render::GL
{
    // GPU skinning shader for BMD models (P-bmd-gpu). Reproduces the legacy
    // BMD::Transform math exactly (pos = Bone[node]*model * BodyScale + BodyOrigin;
    // lum = max(dot(rotate(normal,Bone),LightPos)*0.8+0.4, 0.2); color =
    // BodyLight*lum) so A/B with $gpubmd is visually identical. Reuses the
    // compat built-in gl_ModelViewProjectionMatrix (the engine's existing per-frame
    // / per-object modelview is untouched).
    //
    // Owns the program + cached uniform/attribute locations. Bone matrices upload
    // as a column-major mat4 array (3x4 affine padded). Lazy-compiled on first use.
    class BmdShader
    {
    public:
        static constexpr int kMaxBones = 96;   // uBones[] size; models beyond -> legacy

        // Compile if needed; returns false (and logs) on failure. Safe to call
        // repeatedly. Requires LoadModernGL() to have succeeded.
        bool Ensure();
        bool Valid() const { return m_prog.Valid(); }

        void Use() const { m_prog.Use(); }

        // Per-draw uniforms. bones = kMaxBones column-major mat4 (16 floats each);
        // boneCount<=kMaxBones rows are meaningful. body scale/origin reproduce the
        // Translate path (1/0 when the modelview already carries the placement).
        void SetBones(const float* mat4ColumnMajor, int boneCount) const;
        void SetBody(float bodyScale, const float bodyOrigin[3]) const;
        void SetLight(const float lightPos[3], const float bodyLight[3], float alpha) const;
        // lit = 1.0 -> per-normal lighting (props); 0.0 -> flat color, uBodyLight is
        // the current glColor (characters render with a single flat color).
        void SetLit(float lit) const;
        void SetTextureUnit(int unit) const;

        // Generic vertex-attribute locations (queried after link). Used by the
        // mesh cache to wire glVertexAttribPointer. -1 if absent.
        GLint AttrPos()    const { return m_aPos; }
        GLint AttrVBone()  const { return m_aVBone; }
        GLint AttrNormal() const { return m_aNormal; }
        GLint AttrNBone()  const { return m_aNBone; }
        GLint AttrUV()     const { return m_aUV; }

    private:
        ShaderProgram m_prog;
        // Cached uniform locations.
        GLint m_uBones = -1, m_uBodyScale = -1, m_uBodyOrigin = -1;
        GLint m_uLightPos = -1, m_uBodyLight = -1, m_uAlpha = -1, m_uTex = -1, m_uLit = -1;
        // Cached attribute locations.
        GLint m_aPos = -1, m_aVBone = -1, m_aNormal = -1, m_aNBone = -1, m_aUV = -1;
        bool  m_tried = false;
    };

    // Process-wide instance (one shader shared by all BMD GPU draws).
    BmdShader& GetBmdShader();
}
