#include "Render/GL/GpuBuffer.h"

#include "Render/GL/GLLoader.h"

namespace Render::GL
{
    GpuBuffer::~GpuBuffer()
    {
        Destroy();
    }

    GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
        : m_id(other.m_id), m_size(other.m_size)
    {
        other.m_id = 0;
        other.m_size = 0;
    }

    GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_id = other.m_id;
            m_size = other.m_size;
            other.m_id = 0;
            other.m_size = 0;
        }
        return *this;
    }

    void GpuBuffer::Destroy()
    {
        if (m_id != 0 && DeleteBuffers != nullptr)
            DeleteBuffers(1, &m_id);
        m_id = 0;
        m_size = 0;
    }

    void GpuBuffer::Upload(GLenum target, const void* data, GLsizeiptr size, GLenum usage)
    {
        if (m_id == 0)
            GenBuffers(1, &m_id);

        BindBuffer(target, m_id);
        // Re-allocate when growing or first upload; otherwise sub-update in place.
        if (size > m_size || usage != GL_STATIC_DRAW)
        {
            BufferData(target, size, data, usage);
            m_size = size;
        }
        else
        {
            BufferSubData(target, 0, size, data);
        }
    }

    void GpuBuffer::Bind(GLenum target) const
    {
        BindBuffer(target, m_id);
    }
}
