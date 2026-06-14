#include "Render/GL/BmdShader.h"

#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"

namespace Render::GL
{
    namespace
    {
        // Vertex: skin position by the vertex's bone, apply BodyScale/BodyOrigin
        // (the legacy Translate path), then the existing fixed-function MVP. Light
        // = rotate(normal, bone) . lightPos, exactly as BMD::Transform.
        const char* kVS =
            "#version 120\n"
            "const int MAX_BONES = 96;\n"
            "uniform mat4 uBones[MAX_BONES];\n"
            "uniform float uBodyScale;\n"
            "uniform vec3  uBodyOrigin;\n"
            "uniform vec3  uLightPos;\n"
            "uniform vec3  uBodyLight;\n"
            "uniform float uAlpha;\n"
            "attribute vec3  aPos;\n"
            "attribute float aVBone;\n"
            "attribute vec3  aNormal;\n"
            "attribute float aNBone;\n"
            "attribute vec2  aUV;\n"
            "varying vec2 vUV;\n"
            "void main() {\n"
            "    mat4 Bv = uBones[int(aVBone)];\n"
            "    vec3 vp = (Bv * vec4(aPos, 1.0)).xyz;\n"
            "    vp = vp * uBodyScale + uBodyOrigin;\n"
            "    gl_Position = gl_ModelViewProjectionMatrix * vec4(vp, 1.0);\n"
            "    mat3 Rn = mat3(uBones[int(aNBone)]);\n"
            "    vec3 tn = Rn * aNormal;\n"
            "    float lum = dot(tn, uLightPos) * 0.8 + 0.4;\n"
            "    lum = max(lum, 0.2);\n"
            "    gl_FrontColor = vec4(uBodyLight * lum, uAlpha);\n"
            "    vUV = aUV;\n"
            "}\n";

        const char* kFS =
            "#version 120\n"
            "uniform sampler2D uTex;\n"
            "varying vec2 vUV;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(uTex, vUV) * gl_Color;\n"
            "}\n";
    }

    bool BmdShader::Ensure()
    {
        if (m_prog.Valid())
            return true;
        if (m_tried)         // already failed once; don't spam compiles
            return false;
        m_tried = true;

        if (!IsLoaded())
            return false;

        if (!m_prog.Compile(kVS, kFS, "bmd_gpu"))
            return false;

        m_uBones      = m_prog.Uniform("uBones");
        m_uBodyScale  = m_prog.Uniform("uBodyScale");
        m_uBodyOrigin = m_prog.Uniform("uBodyOrigin");
        m_uLightPos   = m_prog.Uniform("uLightPos");
        m_uBodyLight  = m_prog.Uniform("uBodyLight");
        m_uAlpha      = m_prog.Uniform("uAlpha");
        m_uTex        = m_prog.Uniform("uTex");

        // Attribute locations are linker-assigned (#version 120 `attribute`); query
        // them so the mesh cache can wire glVertexAttribPointer to the right slots.
        const GLuint id = m_prog.Id();
        m_aPos    = GetAttribLocation(id, "aPos");
        m_aVBone  = GetAttribLocation(id, "aVBone");
        m_aNormal = GetAttribLocation(id, "aNormal");
        m_aNBone  = GetAttribLocation(id, "aNBone");
        m_aUV     = GetAttribLocation(id, "aUV");

        Log("[bmd_gpu] shader ready: uBones=%d body=%d/%d light=%d/%d/%d tex=%d | "
            "attr pos=%d vbone=%d nrm=%d nbone=%d uv=%d",
            m_uBones, m_uBodyScale, m_uBodyOrigin, m_uLightPos, m_uBodyLight, m_uAlpha, m_uTex,
            m_aPos, m_aVBone, m_aNormal, m_aNBone, m_aUV);
        return true;
    }

    void BmdShader::SetBones(const float* mat4ColumnMajor, int boneCount) const
    {
        if (m_uBones < 0 || mat4ColumnMajor == nullptr)
            return;
        if (boneCount < 1) boneCount = 1;
        if (boneCount > kMaxBones) boneCount = kMaxBones;
        UniformMatrix4fv(m_uBones, boneCount, GL_FALSE, mat4ColumnMajor);
    }

    void BmdShader::SetBody(float bodyScale, const float bodyOrigin[3]) const
    {
        if (m_uBodyScale >= 0)  Uniform1f(m_uBodyScale, bodyScale);
        if (m_uBodyOrigin >= 0) Uniform3fv(m_uBodyOrigin, 1, bodyOrigin);
    }

    void BmdShader::SetLight(const float lightPos[3], const float bodyLight[3], float alpha) const
    {
        if (m_uLightPos >= 0)  Uniform3fv(m_uLightPos, 1, lightPos);
        if (m_uBodyLight >= 0) Uniform3fv(m_uBodyLight, 1, bodyLight);
        if (m_uAlpha >= 0)     Uniform1f(m_uAlpha, alpha);
    }

    void BmdShader::SetTextureUnit(int unit) const
    {
        if (m_uTex >= 0) Uniform1i(m_uTex, unit);
    }

    BmdShader& GetBmdShader()
    {
        static BmdShader s_shader;
        return s_shader;
    }
}
