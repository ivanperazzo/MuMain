#pragma once

#include "Render/GL/ShaderProgram.h"

#include <gl/glew.h>

namespace Render::GL
{
    // Instanced GPU-skinned SHADOW shader (P-bmd-shadow). Draws every character's
    // shadow triangles in one glDrawArraysInstanced per (model, mesh), replacing the
    // legacy per-vertex CPU path (BMD::AddMeshShadowTriangles -> CalcShadowPosition ->
    // RequestTerrainHeight per vertex + immediate draw), which measured ~16ms of the
    // ~36ms character cost at 100 chars (the single biggest CPU hog; the GPU is idle).
    //
    // The vertex shader skins the position from the BonePaletteTBO exactly like
    // InstancedBmdShader (vp = boneMatrix*aPos*bodyScale + bodyOrigin == VertexTransform),
    // then applies CalcShadowPosition's flatten in clip-space coords:
    //   r  = vp - origin;  r.x += r.z*(r.x+sx)/(r.z-sy);  r += origin;  r.z = groundZ;
    // groundZ is RequestTerrainHeight(origin) + 5, sampled ONCE per character on the CPU
    // (vs once per vertex in the legacy path) and passed as a per-instance attribute.
    //   - per-vertex attribs (divisor 0): aPos/aVBone (shared model geometry VBO).
    //   - per-instance attribs (divisor 1): aPaletteBase/aBodyScale/aBodyOrigin/aGroundZ.
    //   - uniforms: uPalette (samplerBuffer), uSX/uSY (frame-global skew), uColor.
    class InstancedShadowShader
    {
    public:
        bool Ensure();
        bool Valid() const { return m_prog.Valid(); }
        void Use() const { m_prog.Use(); }

        void SetPaletteUnit(int unit) const;          // samplerBuffer texture unit
        void SetSkew(float sx, float sy) const;       // CalcShadowPosition sx/sy
        void SetColor(const float rgba[4]) const;     // shadow colour (0,0,0,0.5)

        // Per-vertex attribute locations (divisor 0).
        GLint AttrPos()   const { return m_aPos; }
        GLint AttrVBone() const { return m_aVBone; }
        // Per-instance attribute locations (divisor 1).
        GLint AttrPaletteBase() const { return m_aPaletteBase; }
        GLint AttrBodyScale()   const { return m_aBodyScale; }
        GLint AttrBodyOrigin()  const { return m_aBodyOrigin; }
        GLint AttrGroundZ()     const { return m_aGroundZ; }

    private:
        ShaderProgram m_prog;
        GLint m_uPalette = -1, m_uSX = -1, m_uSY = -1, m_uColor = -1;
        GLint m_aPos = -1, m_aVBone = -1;
        GLint m_aPaletteBase = -1, m_aBodyScale = -1, m_aBodyOrigin = -1, m_aGroundZ = -1;
        bool  m_tried = false;
    };

    InstancedShadowShader& GetInstancedShadowShader();
}
