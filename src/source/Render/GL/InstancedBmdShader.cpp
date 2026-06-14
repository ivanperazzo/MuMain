#include "Render/GL/InstancedBmdShader.h"

#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"

namespace Render::GL
{
    namespace
    {
        // #version 150 compatibility: needs texelFetch(samplerBuffer) (130+) AND the
        // compat built-in gl_ModelViewProjectionMatrix (the shared camera view).
        // Bone matrix = 3 texels (rows of the 3x4 affine): pos = dot(row.xyz,p)+row.w.
        const char* kVS =
            "#version 150 compatibility\n"
            "uniform samplerBuffer uPalette;\n"
            "uniform vec3 uLightPos;\n"
            "uniform int  uChromeMode;\n"   // 0=textured(aUV) 1=CHROME 2=CHROME2 3=CHROME3 4=CHROME4 6=CHROME6 8=METAL
            "uniform float uWave;\n"        // chrome reflection scroll (WorldTime-based)
            "uniform float uWave2;\n"       // CHROME2/6 scroll (WorldTime%5000*0.00024-0.4)
            "uniform vec3  uChromeL;\n"     // CHROME4 light vec (cos t*.001, sin t*.002, 1)
            "uniform vec3  uChromeLightVec;\n" // CHROME3 LightVector
            "in vec3  aPos;\n"
            "in float aVBone;\n"
            "in vec3  aNormal;\n"
            "in float aNBone;\n"
            "in vec2  aUV;\n"
            "in float aPaletteBase;\n"
            "in float aBodyScale;\n"
            "in vec3  aBodyOrigin;\n"
            "in vec4  aColor;\n"
            "in float aLit;\n"
            "out vec2 vUV;\n"
            "out vec4 vColor;\n"
            "void main() {\n"
            "    int vb = (int(aPaletteBase) + int(aVBone)) * 3;\n"
            "    vec4 r0 = texelFetch(uPalette, vb + 0);\n"
            "    vec4 r1 = texelFetch(uPalette, vb + 1);\n"
            "    vec4 r2 = texelFetch(uPalette, vb + 2);\n"
            "    vec3 vp = vec3(dot(r0.xyz, aPos) + r0.w,\n"
            "                   dot(r1.xyz, aPos) + r1.w,\n"
            "                   dot(r2.xyz, aPos) + r2.w);\n"
            "    vp = vp * aBodyScale + aBodyOrigin;\n"
            "    gl_Position = gl_ModelViewProjectionMatrix * vec4(vp, 1.0);\n"
            "    if (uChromeMode > 0) {\n"
            // sphere-map texcoord from the skinned normal; per-variant formula matching
            // legacy BMD::RenderMesh (g_chrome[]). Body light modulates the reflection.
            "        int nb = (int(aPaletteBase) + int(aNBone)) * 3;\n"
            "        vec3 n = vec3(dot(texelFetch(uPalette, nb + 0).xyz, aNormal),\n"
            "                      dot(texelFetch(uPalette, nb + 1).xyz, aNormal),\n"
            "                      dot(texelFetch(uPalette, nb + 2).xyz, aNormal));\n"
            "        vec2 uv;\n"
            "        if (uChromeMode == 2) {\n"            // CHROME2
            "            uv = vec2((n.z+n.x)*0.8 + uWave2*2.0, (n.y+n.x)*1.0 + uWave2*3.0);\n"
            "        } else if (uChromeMode == 3) {\n"     // CHROME3
            "            float d = dot(n, uChromeLightVec); uv = vec2(d, 1.0 - d);\n"
            "        } else if (uChromeMode == 4) {\n"     // CHROME4
            "            float d = dot(n, uChromeL);\n"
            "            uv = vec2(d + n.y*0.5 + uChromeL.y*3.0, (1.0 - d) - n.z*0.5 - uWave*3.0);\n"
            "        } else if (uChromeMode == 6) {\n"     // CHROME6
            "            float s = (n.z+n.x)*0.8 + uWave2*2.0; uv = vec2(s, s);\n"
            "        } else if (uChromeMode == 8) {\n"     // METAL (legacy else formula)
            "            uv = vec2(n.z*0.5 + 0.2, n.y*0.5 + 0.5);\n"
            "        } else {\n"                            // CHROME (plain)
            "            uv = vec2(n.z*0.5 + uWave, n.y*0.5 + uWave*2.0);\n"
            "        }\n"
            "        vUV = uv;\n"
            "        vColor = aColor;\n"
            "    } else {\n"
            "        float lum = 1.0;\n"
            "        if (aLit > 0.5) {\n"
            "            int nb = (int(aPaletteBase) + int(aNBone)) * 3;\n"
            "            vec3 tn = vec3(dot(texelFetch(uPalette, nb + 0).xyz, aNormal),\n"
            "                           dot(texelFetch(uPalette, nb + 1).xyz, aNormal),\n"
            "                           dot(texelFetch(uPalette, nb + 2).xyz, aNormal));\n"
            "            lum = max(dot(tn, uLightPos) * 0.8 + 0.4, 0.2);\n"
            "        }\n"
            "        vColor = vec4(aColor.rgb * lum, aColor.a);\n"
            "        vUV = aUV;\n"
            "    }\n"
            "}\n";

        const char* kFS =
            "#version 150 compatibility\n"
            "uniform sampler2D uTex;\n"
            "in vec2 vUV;\n"
            "in vec4 vColor;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    fragColor = texture(uTex, vUV) * vColor;\n"
            "}\n";
    }

    bool InstancedBmdShader::Ensure()
    {
        if (m_prog.Valid())
            return true;
        if (m_tried)
            return false;
        m_tried = true;

        if (!IsLoaded())
            return false;
        if (!m_prog.Compile(kVS, kFS, "bmd_inst"))
            return false;

        m_uLightPos   = m_prog.Uniform("uLightPos");
        m_uPalette    = m_prog.Uniform("uPalette");
        m_uTex        = m_prog.Uniform("uTex");
        m_uChromeMode = m_prog.Uniform("uChromeMode");
        m_uWave       = m_prog.Uniform("uWave");
        m_uWave2      = m_prog.Uniform("uWave2");
        m_uChromeL    = m_prog.Uniform("uChromeL");
        m_uChromeLightVec = m_prog.Uniform("uChromeLightVec");

        const GLuint id = m_prog.Id();
        m_aPos         = GetAttribLocation(id, "aPos");
        m_aVBone       = GetAttribLocation(id, "aVBone");
        m_aNormal      = GetAttribLocation(id, "aNormal");
        m_aNBone       = GetAttribLocation(id, "aNBone");
        m_aUV          = GetAttribLocation(id, "aUV");
        m_aPaletteBase = GetAttribLocation(id, "aPaletteBase");
        m_aBodyScale   = GetAttribLocation(id, "aBodyScale");
        m_aBodyOrigin  = GetAttribLocation(id, "aBodyOrigin");
        m_aColor       = GetAttribLocation(id, "aColor");
        m_aLit         = GetAttribLocation(id, "aLit");

        Log("[bmd_inst] ready: uPalette=%d uLight=%d uTex=%d | vtx pos=%d vb=%d nrm=%d nb=%d uv=%d "
            "| inst base=%d scale=%d origin=%d color=%d lit=%d",
            m_uPalette, m_uLightPos, m_uTex,
            m_aPos, m_aVBone, m_aNormal, m_aNBone, m_aUV,
            m_aPaletteBase, m_aBodyScale, m_aBodyOrigin, m_aColor, m_aLit);
        return true;
    }

    void InstancedBmdShader::SetLight(const float lightPos[3]) const
    {
        if (m_uLightPos >= 0) Uniform3fv(m_uLightPos, 1, lightPos);
    }

    void InstancedBmdShader::SetPaletteUnit(int unit) const
    {
        if (m_uPalette >= 0) Uniform1i(m_uPalette, unit);
    }

    void InstancedBmdShader::SetTextureUnit(int unit) const
    {
        if (m_uTex >= 0) Uniform1i(m_uTex, unit);
    }

    void InstancedBmdShader::SetChromeMode(int mode) const
    {
        if (m_uChromeMode >= 0) Uniform1i(m_uChromeMode, mode);
    }

    void InstancedBmdShader::SetWave(float wave) const
    {
        if (m_uWave >= 0) Uniform1f(m_uWave, wave);
    }

    void InstancedBmdShader::SetChromeParams(float wave2, const float L[3], const float lightVec[3]) const
    {
        if (m_uWave2 >= 0)         Uniform1f(m_uWave2, wave2);
        if (m_uChromeL >= 0)       Uniform3fv(m_uChromeL, 1, L);
        if (m_uChromeLightVec >= 0) Uniform3fv(m_uChromeLightVec, 1, lightVec);
    }

    InstancedBmdShader& GetInstancedBmdShader()
    {
        static InstancedBmdShader s_shader;
        return s_shader;
    }
}
