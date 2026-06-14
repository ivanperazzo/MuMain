#include "Render/GL/ShaderProgram.h"

#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"

namespace Render::GL
{
    namespace
    {
        // Compile one stage; logs the info log and returns 0 on failure.
        GLuint CompileStage(GLenum type, const char* src, const char* name, const char* stage)
        {
            GLuint sh = CreateShader(type);
            if (sh == 0)
            {
                Log("[shader] %s: glCreateShader(%s) returned 0", name, stage);
                return 0;
            }

            ShaderSource(sh, 1, &src, nullptr);
            CompileShader(sh);

            GLint ok = 0;
            GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE)
            {
                char info[2048] = {};
                GLsizei len = 0;
                GetShaderInfoLog(sh, sizeof(info) - 1, &len, info);
                Log("[shader] %s: %s compile FAILED:\n%s", name, stage, info);
                DeleteShader(sh);
                return 0;
            }
            return sh;
        }
    }

    ShaderProgram::~ShaderProgram()
    {
        if (m_program != 0 && DeleteProgram != nullptr)
            DeleteProgram(m_program);
        m_program = 0;
    }

    bool ShaderProgram::Compile(const char* vertexSrc, const char* fragmentSrc, const char* name)
    {
        if (!IsLoaded())
        {
            Log("[shader] %s: modern GL not loaded", name);
            return false;
        }

        GLuint vs = CompileStage(GL_VERTEX_SHADER, vertexSrc, name, "vertex");
        if (vs == 0)
            return false;

        GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragmentSrc, name, "fragment");
        if (fs == 0)
        {
            DeleteShader(vs);
            return false;
        }

        GLuint prog = CreateProgram();
        AttachShader(prog, vs);
        AttachShader(prog, fs);
        LinkProgram(prog);

        // Shaders can be deleted after link (the program keeps them alive).
        DetachShader(prog, vs);
        DetachShader(prog, fs);
        DeleteShader(vs);
        DeleteShader(fs);

        GLint ok = 0;
        GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok == GL_FALSE)
        {
            char info[2048] = {};
            GLsizei len = 0;
            GetProgramInfoLog(prog, sizeof(info) - 1, &len, info);
            Log("[shader] %s: link FAILED:\n%s", name, info);
            DeleteProgram(prog);
            return false;
        }

        if (m_program != 0)
            DeleteProgram(m_program);
        m_program = prog;
        Log("[shader] %s: compiled+linked OK (program=%u)", name, m_program);
        return true;
    }

    void ShaderProgram::Use() const
    {
        if (m_program != 0)
            UseProgram(m_program);
    }

    GLint ShaderProgram::Uniform(const char* name) const
    {
        if (m_program == 0)
            return -1;
        return GetUniformLocation(m_program, name);
    }
}
