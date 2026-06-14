#include "Render/GL/InstancedShadowShader.h"

#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"

namespace Render::GL
{
    namespace
    {
        // #version 150 compatibility: texelFetch(samplerBuffer) (130+) + the compat
        // gl_ModelViewProjectionMatrix (shared camera view, valid because the world
        // placement is baked into bodyOrigin).
        const char* kVS =
            "#version 150 compatibility\n"
            "uniform samplerBuffer uPalette;\n"
            "uniform float uSX;\n"
            "uniform float uSY;\n"
            "in vec3  aPos;\n"
            "in float aVBone;\n"
            "in float aPaletteBase;\n"
            "in float aBodyScale;\n"
            "in vec3  aBodyOrigin;\n"
            "in float aGroundZ;\n"
            "void main() {\n"
            // Skin: identical to InstancedBmdShader -> vp is the world VertexTransform.
            "    int vb = (int(aPaletteBase) + int(aVBone)) * 3;\n"
            "    vec4 r0 = texelFetch(uPalette, vb + 0);\n"
            "    vec4 r1 = texelFetch(uPalette, vb + 1);\n"
            "    vec4 r2 = texelFetch(uPalette, vb + 2);\n"
            "    vec3 vp = vec3(dot(r0.xyz, aPos) + r0.w,\n"
            "                   dot(r1.xyz, aPos) + r1.w,\n"
            "                   dot(r2.xyz, aPos) + r2.w);\n"
            "    vp = vp * aBodyScale + aBodyOrigin;\n"
            // CalcShadowPosition: relative to origin, x-skew by height, snap z to ground.
            "    vec3 r = vp - aBodyOrigin;\n"
            "    r.x += r.z * (r.x + uSX) / (r.z - uSY);\n"
            "    r += aBodyOrigin;\n"
            "    r.z = aGroundZ;\n"
            "    gl_Position = gl_ModelViewProjectionMatrix * vec4(r, 1.0);\n"
            "}\n";

        const char* kFS =
            "#version 150 compatibility\n"
            "uniform vec4 uColor;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    fragColor = uColor;\n"
            "}\n";
    }

    bool InstancedShadowShader::Ensure()
    {
        if (m_prog.Valid())
            return true;
        if (m_tried)
            return false;
        m_tried = true;

        if (!IsLoaded())
            return false;
        if (!m_prog.Compile(kVS, kFS, "bmd_shadow"))
            return false;

        m_uPalette = m_prog.Uniform("uPalette");
        m_uSX      = m_prog.Uniform("uSX");
        m_uSY      = m_prog.Uniform("uSY");
        m_uColor   = m_prog.Uniform("uColor");

        const GLuint id = m_prog.Id();
        m_aPos         = GetAttribLocation(id, "aPos");
        m_aVBone       = GetAttribLocation(id, "aVBone");
        m_aPaletteBase = GetAttribLocation(id, "aPaletteBase");
        m_aBodyScale   = GetAttribLocation(id, "aBodyScale");
        m_aBodyOrigin  = GetAttribLocation(id, "aBodyOrigin");
        m_aGroundZ     = GetAttribLocation(id, "aGroundZ");

        Log("[bmd_shadow] ready: uPalette=%d uSX=%d uSY=%d uColor=%d | vtx pos=%d vb=%d "
            "| inst base=%d scale=%d origin=%d groundZ=%d",
            m_uPalette, m_uSX, m_uSY, m_uColor,
            m_aPos, m_aVBone, m_aPaletteBase, m_aBodyScale, m_aBodyOrigin, m_aGroundZ);
        return true;
    }

    void InstancedShadowShader::SetPaletteUnit(int unit) const
    {
        if (m_uPalette >= 0) Uniform1i(m_uPalette, unit);
    }

    void InstancedShadowShader::SetSkew(float sx, float sy) const
    {
        if (m_uSX >= 0) Uniform1f(m_uSX, sx);
        if (m_uSY >= 0) Uniform1f(m_uSY, sy);
    }

    void InstancedShadowShader::SetColor(const float rgba[4]) const
    {
        if (m_uColor >= 0) Uniform4fv(m_uColor, 1, rgba);
    }

    InstancedShadowShader& GetInstancedShadowShader()
    {
        static InstancedShadowShader s_shader;
        return s_shader;
    }
}
