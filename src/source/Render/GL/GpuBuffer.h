#pragma once

#include <gl/glew.h>   // GL types (GLuint/GLenum/GLsizeiptr)

namespace Render::GL
{
    // RAII wrapper over a GL buffer object (VBO/IBO). Geometry uploaded once lives
    // in GPU memory, so draws no longer re-specify client-side arrays every frame
    // (the core of the BMD-to-GPU win). Non-copyable, movable. Uses Render::GL
    // function pointers -- create only after LoadModernGL().
    class GpuBuffer
    {
    public:
        GpuBuffer() = default;
        ~GpuBuffer();

        GpuBuffer(const GpuBuffer&)            = delete;
        GpuBuffer& operator=(const GpuBuffer&) = delete;
        GpuBuffer(GpuBuffer&& other) noexcept;
        GpuBuffer& operator=(GpuBuffer&& other) noexcept;

        // Allocate (if needed) and upload `size` bytes. target = GL_ARRAY_BUFFER
        // or GL_ELEMENT_ARRAY_BUFFER; usage = GL_STATIC_DRAW / GL_DYNAMIC_DRAW.
        void Upload(GLenum target, const void* data, GLsizeiptr size, GLenum usage);

        void Bind(GLenum target) const;
        bool Valid() const { return m_id != 0; }
        GLuint Id() const  { return m_id; }

    private:
        void Destroy();
        GLuint     m_id   = 0;
        GLsizeiptr m_size = 0;
    };
}
