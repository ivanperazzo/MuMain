#include "Render/GL/GLLoader.h"

#include "Render/GL/ShaderProgram.h"
#include "Render/GL/GpuBuffer.h"
#include "Render/GL/GLLog.h"

#include <gl/glew.h>

namespace Render::GL
{
    namespace
    {
        // Representative shader for the BMD-to-GPU work. Uses compatibility-profile
        // built-ins (gl_ModelViewProjectionMatrix, gl_Vertex, gl_Color,
        // gl_MultiTexCoord0) so it reuses the engine's existing fixed-function
        // matrix + per-vertex color/texcoord setup -- the first BMD GPU shader can
        // start here and add bone-matrix skinning incrementally.
        const char* kSelfTestVS =
            "#version 120\n"
            "varying vec2 vUV;\n"
            "void main() {\n"
            "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
            "    vUV = gl_MultiTexCoord0.xy;\n"
            "    gl_FrontColor = gl_Color;\n"
            "}\n";

        const char* kSelfTestFS =
            "#version 120\n"
            "uniform sampler2D uTex;\n"
            "varying vec2 vUV;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(uTex, vUV) * gl_Color;\n"
            "}\n";
    }

    bool SelfTest()
    {
        if (!IsLoaded())
        {
            Log("[selftest] skipped: modern GL not loaded");
            return false;
        }

        ShaderProgram prog;
        const bool shaderOk = prog.Compile(kSelfTestVS, kSelfTestFS, "selftest");

        GpuBuffer vbo;
        const float verts[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f };
        vbo.Upload(GL_ARRAY_BUFFER, verts, sizeof(verts), GL_STATIC_DRAW);
        const bool vboOk = vbo.Valid();

        const GLint uTex = prog.Uniform("uTex");

        Log("[selftest] shader=%s vbo=%s uTexLoc=%d => %s",
            shaderOk ? "OK" : "FAIL",
            vboOk ? "OK" : "FAIL",
            uTex,
            (shaderOk && vboOk) ? "PIPELINE OK" : "PIPELINE FAIL");

        return shaderOk && vboOk;
    }
}
