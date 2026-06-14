#pragma once

#include <gl/glew.h>   // GL types (GLuint/GLint/GLchar)

namespace Render::GL
{
    // A linked GLSL program (vertex + fragment). Compiles from in-memory source,
    // routing compile/link errors to gl_log.txt (stderr is not captured in this
    // GUI app). RAII: deletes the program on destruction. Non-copyable.
    //
    // Uses the Render::GL function pointers (loaded by GLLoader); construct/compile
    // only after LoadModernGL() succeeded.
    class ShaderProgram
    {
    public:
        ShaderProgram() = default;
        ~ShaderProgram();

        ShaderProgram(const ShaderProgram&)            = delete;
        ShaderProgram& operator=(const ShaderProgram&) = delete;

        // Compile + link. `name` is used only in log messages. Returns false and
        // logs the GLSL info log on any failure (program left invalid).
        bool Compile(const char* vertexSrc, const char* fragmentSrc, const char* name);

        void Use() const;                 // glUseProgram(this) -- no-op if invalid
        bool Valid() const { return m_program != 0; }
        GLuint Id() const  { return m_program; }

        // Uniform location (-1 if absent/invalid). Not cached -- callers should
        // resolve once and store for hot paths.
        GLint Uniform(const char* name) const;

    private:
        GLuint m_program = 0;
    };
}
