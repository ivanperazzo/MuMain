#include "Render/GL/GLLoader.h"

#include <SDL3/SDL.h>
#include <gl/GL.h>
#include <cstdio>

namespace Render::GL
{
    // Definitions (declared extern in the header).
    PFNGLGENBUFFERSPROC            GenBuffers            = nullptr;
    PFNGLBINDBUFFERPROC            BindBuffer            = nullptr;
    PFNGLBUFFERDATAPROC            BufferData            = nullptr;
    PFNGLBUFFERSUBDATAPROC         BufferSubData         = nullptr;
    PFNGLDELETEBUFFERSPROC         DeleteBuffers         = nullptr;

    PFNGLGENVERTEXARRAYSPROC       GenVertexArrays       = nullptr;
    PFNGLBINDVERTEXARRAYPROC       BindVertexArray       = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC    DeleteVertexArrays    = nullptr;

    PFNGLCREATESHADERPROC          CreateShader          = nullptr;
    PFNGLSHADERSOURCEPROC          ShaderSource          = nullptr;
    PFNGLCOMPILESHADERPROC         CompileShader         = nullptr;
    PFNGLGETSHADERIVPROC           GetShaderiv           = nullptr;
    PFNGLGETSHADERINFOLOGPROC      GetShaderInfoLog      = nullptr;
    PFNGLATTACHSHADERPROC          AttachShader          = nullptr;
    PFNGLDETACHSHADERPROC          DetachShader          = nullptr;
    PFNGLCREATEPROGRAMPROC         CreateProgram         = nullptr;
    PFNGLLINKPROGRAMPROC           LinkProgram           = nullptr;
    PFNGLGETPROGRAMIVPROC          GetProgramiv          = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC     GetProgramInfoLog     = nullptr;
    PFNGLUSEPROGRAMPROC            UseProgram            = nullptr;
    PFNGLDELETESHADERPROC          DeleteShader          = nullptr;
    PFNGLDELETEPROGRAMPROC         DeleteProgram         = nullptr;

    PFNGLGETUNIFORMLOCATIONPROC    GetUniformLocation    = nullptr;
    PFNGLUNIFORM1IPROC             Uniform1i             = nullptr;
    PFNGLUNIFORM1FPROC             Uniform1f             = nullptr;
    PFNGLUNIFORM3FVPROC            Uniform3fv            = nullptr;
    PFNGLUNIFORM4FVPROC            Uniform4fv            = nullptr;
    PFNGLUNIFORMMATRIX4FVPROC      UniformMatrix4fv      = nullptr;
    PFNGLGETATTRIBLOCATIONPROC     GetAttribLocation     = nullptr;
    PFNGLBINDATTRIBLOCATIONPROC    BindAttribLocation    = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC   VertexAttribPointer   = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC  EnableVertexAttribArray  = nullptr;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray = nullptr;

    PFNGLACTIVETEXTUREPROC         ActiveTexture         = nullptr;

    namespace
    {
        bool s_loaded = false;
        int  s_failures = 0;

        // Resolve one entry point; count + name any miss for the log.
        template <typename T>
        void Load(T& fn, const char* name)
        {
            fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
            if (fn == nullptr)
            {
                ++s_failures;
                std::fprintf(stderr, "[GL] missing entry point: %s\n", name);
            }
        }
    }

    bool LoadModernGL()
    {
        s_failures = 0;

        Load(GenBuffers,            "glGenBuffers");
        Load(BindBuffer,            "glBindBuffer");
        Load(BufferData,            "glBufferData");
        Load(BufferSubData,         "glBufferSubData");
        Load(DeleteBuffers,         "glDeleteBuffers");

        Load(GenVertexArrays,       "glGenVertexArrays");
        Load(BindVertexArray,       "glBindVertexArray");
        Load(DeleteVertexArrays,    "glDeleteVertexArrays");

        Load(CreateShader,          "glCreateShader");
        Load(ShaderSource,          "glShaderSource");
        Load(CompileShader,         "glCompileShader");
        Load(GetShaderiv,           "glGetShaderiv");
        Load(GetShaderInfoLog,      "glGetShaderInfoLog");
        Load(AttachShader,          "glAttachShader");
        Load(DetachShader,          "glDetachShader");
        Load(CreateProgram,         "glCreateProgram");
        Load(LinkProgram,           "glLinkProgram");
        Load(GetProgramiv,          "glGetProgramiv");
        Load(GetProgramInfoLog,     "glGetProgramInfoLog");
        Load(UseProgram,            "glUseProgram");
        Load(DeleteShader,          "glDeleteShader");
        Load(DeleteProgram,         "glDeleteProgram");

        Load(GetUniformLocation,    "glGetUniformLocation");
        Load(Uniform1i,             "glUniform1i");
        Load(Uniform1f,             "glUniform1f");
        Load(Uniform3fv,            "glUniform3fv");
        Load(Uniform4fv,            "glUniform4fv");
        Load(UniformMatrix4fv,      "glUniformMatrix4fv");
        Load(GetAttribLocation,     "glGetAttribLocation");
        Load(BindAttribLocation,    "glBindAttribLocation");
        Load(VertexAttribPointer,   "glVertexAttribPointer");
        Load(EnableVertexAttribArray,  "glEnableVertexAttribArray");
        Load(DisableVertexAttribArray, "glDisableVertexAttribArray");

        Load(ActiveTexture,         "glActiveTexture");

        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* glsl = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        std::fprintf(stderr,
            "[GL] version=%s | renderer=%s | glsl=%s | modern-GL %s (%d missing)\n",
            ver ? ver : "?", ren ? ren : "?", glsl ? glsl : "?",
            s_failures == 0 ? "LOADED" : "INCOMPLETE", s_failures);
        std::fflush(stderr);

        s_loaded = (s_failures == 0);
        return s_loaded;
    }

    bool IsLoaded() { return s_loaded; }
}
