#pragma once

// Modern-GL function loader (GPU/high-FPS track, P-infra). The client links only
// opengl32 (GL 1.1) and never calls glewInit, so VBO (GL 1.5+) and shader (2.0+)
// entry points are unresolved. The context is SDL-created, so we load them via
// SDL_GL_GetProcAddress. On a compatibility-profile context (GL 4.6 on this GPU)
// these coexist with the legacy immediate-mode path, enabling incremental
// per-subsystem migration to GPU geometry.
//
// Pointers are namespaced WITHOUT the "gl" prefix (Render::GL::GenBuffers, not
// glGenBuffers) so they never collide with GLEW's `#define glGenBuffers ...`
// macros from the globally-included glew.h. New GPU code calls Render::GL::X(...).
// The PFNGL*PROC typedefs come from glew.h (header-only; no link/init needed).

#include <gl/glew.h>   // PFNGL*PROC typedefs only

namespace Render::GL
{
    // Buffers
    extern PFNGLGENBUFFERSPROC            GenBuffers;
    extern PFNGLBINDBUFFERPROC            BindBuffer;
    extern PFNGLBUFFERDATAPROC            BufferData;
    extern PFNGLBUFFERSUBDATAPROC         BufferSubData;
    extern PFNGLDELETEBUFFERSPROC         DeleteBuffers;

    // Vertex array objects
    extern PFNGLGENVERTEXARRAYSPROC       GenVertexArrays;
    extern PFNGLBINDVERTEXARRAYPROC       BindVertexArray;
    extern PFNGLDELETEVERTEXARRAYSPROC    DeleteVertexArrays;

    // Shaders / programs
    extern PFNGLCREATESHADERPROC          CreateShader;
    extern PFNGLSHADERSOURCEPROC          ShaderSource;
    extern PFNGLCOMPILESHADERPROC         CompileShader;
    extern PFNGLGETSHADERIVPROC           GetShaderiv;
    extern PFNGLGETSHADERINFOLOGPROC      GetShaderInfoLog;
    extern PFNGLATTACHSHADERPROC          AttachShader;
    extern PFNGLDETACHSHADERPROC          DetachShader;
    extern PFNGLCREATEPROGRAMPROC         CreateProgram;
    extern PFNGLLINKPROGRAMPROC           LinkProgram;
    extern PFNGLGETPROGRAMIVPROC          GetProgramiv;
    extern PFNGLGETPROGRAMINFOLOGPROC     GetProgramInfoLog;
    extern PFNGLUSEPROGRAMPROC            UseProgram;
    extern PFNGLDELETESHADERPROC          DeleteShader;
    extern PFNGLDELETEPROGRAMPROC         DeleteProgram;

    // Uniforms / vertex attributes
    extern PFNGLGETUNIFORMLOCATIONPROC    GetUniformLocation;
    extern PFNGLUNIFORM1IPROC             Uniform1i;
    extern PFNGLUNIFORM1FPROC             Uniform1f;
    extern PFNGLUNIFORM2FVPROC            Uniform2fv;
    extern PFNGLUNIFORM3FVPROC            Uniform3fv;
    extern PFNGLUNIFORM4FVPROC            Uniform4fv;
    extern PFNGLUNIFORMMATRIX4FVPROC      UniformMatrix4fv;
    extern PFNGLGETATTRIBLOCATIONPROC     GetAttribLocation;
    extern PFNGLBINDATTRIBLOCATIONPROC    BindAttribLocation;
    extern PFNGLVERTEXATTRIBPOINTERPROC   VertexAttribPointer;
    extern PFNGLENABLEVERTEXATTRIBARRAYPROC  EnableVertexAttribArray;
    extern PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;

    // Texturing
    extern PFNGLACTIVETEXTUREPROC         ActiveTexture;

    // Instancing + texture buffers (GL 3.1+) -- P-bmd-instance. Used to draw many
    // characters in one call with per-instance attributes and a bone-palette TBO.
    extern PFNGLDRAWARRAYSINSTANCEDPROC   DrawArraysInstanced;
    extern PFNGLVERTEXATTRIBDIVISORPROC   VertexAttribDivisor;
    extern PFNGLTEXBUFFERPROC             TexBuffer;

    // Resolve every pointer above via SDL_GL_GetProcAddress. Must be called once,
    // AFTER a GL context is current. Returns true only if all required pointers
    // resolved. Logs GL_VERSION/GL_RENDERER and the loaded count to stderr.
    bool LoadModernGL();

    // True after a successful LoadModernGL(); GPU paths must check this and fall
    // back to the legacy renderer when false.
    bool IsLoaded();

    // Smoke-test the modern pipeline on the real driver: compile a representative
    // shader (uses compat built-in matrices, so it slots into the existing
    // fixed-function setup) and create a VBO. Logs the result to gl_log.txt.
    // Returns true if both succeed. Call once after LoadModernGL().
    bool SelfTest();
}
