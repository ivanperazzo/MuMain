#pragma once

#include "Render/GL/ShaderProgram.h"

#include <gl/glew.h>

namespace Render::GL
{
    // Instanced GPU-skinning shader for BMD meshes (P-bmd-instance). One
    // glDrawArraysInstanced draws many characters that share a (mesh, texture):
    //   - per-vertex attribs (divisor 0): aPos/aVBone/aNormal/aNBone/aUV (the base
    //     model geometry, from BmdGpuCache's VBO).
    //   - per-instance attribs (divisor 1): aPaletteBase/aBodyScale/aBodyOrigin/
    //     aColor/aLit (one record per character).
    //   - bone matrices fetched from the BonePaletteTBO (samplerBuffer) by index.
    // Reproduces BMD::Transform's math exactly (same as BmdShader) so it is A/B
    // identical. Reuses the compat gl_ModelViewProjectionMatrix (the shared camera
    // view, valid because characters bake world placement into BodyOrigin).
    class InstancedBmdShader
    {
    public:
        bool Ensure();
        bool Valid() const { return m_prog.Valid(); }
        void Use() const { m_prog.Use(); }

        void SetLight(const float lightPos[3]) const;
        void SetPaletteUnit(int unit) const;   // samplerBuffer texture unit
        void SetTextureUnit(int unit) const;   // sampler2D texture unit

        // Per-vertex attribute locations (divisor 0).
        GLint AttrPos()    const { return m_aPos; }
        GLint AttrVBone()  const { return m_aVBone; }
        GLint AttrNormal() const { return m_aNormal; }
        GLint AttrNBone()  const { return m_aNBone; }
        GLint AttrUV()     const { return m_aUV; }
        // Per-instance attribute locations (divisor 1).
        GLint AttrPaletteBase() const { return m_aPaletteBase; }
        GLint AttrBodyScale()   const { return m_aBodyScale; }
        GLint AttrBodyOrigin()  const { return m_aBodyOrigin; }
        GLint AttrColor()       const { return m_aColor; }
        GLint AttrLit()         const { return m_aLit; }

    private:
        ShaderProgram m_prog;
        GLint m_uLightPos = -1, m_uPalette = -1, m_uTex = -1;
        GLint m_aPos = -1, m_aVBone = -1, m_aNormal = -1, m_aNBone = -1, m_aUV = -1;
        GLint m_aPaletteBase = -1, m_aBodyScale = -1, m_aBodyOrigin = -1, m_aColor = -1, m_aLit = -1;
        bool  m_tried = false;
    };

    InstancedBmdShader& GetInstancedBmdShader();
}
