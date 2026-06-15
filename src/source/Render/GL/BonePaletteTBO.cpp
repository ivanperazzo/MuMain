#include "Render/GL/BonePaletteTBO.h"

#include "Render/GL/GLLoader.h"
#include "Render/GL/GLLog.h"

#include <gl/GL.h>

namespace Render::GL
{
    BonePaletteTBO::~BonePaletteTBO() { Destroy(); }

    void BonePaletteTBO::Destroy()
    {
        if (m_tex) { glDeleteTextures(1, &m_tex); m_tex = 0; }
        if (m_buf && DeleteBuffers) { DeleteBuffers(1, &m_buf); m_buf = 0; }
        m_capacityBytes = 0;
    }

    bool BonePaletteTBO::Ensure()
    {
        if (m_tex != 0)
            return true;
        if (!IsLoaded() || GenBuffers == nullptr || TexBuffer == nullptr)
            return false;

        GenBuffers(1, &m_buf);
        glGenTextures(1, &m_tex);
        if (m_buf == 0 || m_tex == 0)
        {
            Log("[tbo] failed to create buffer/texture (buf=%u tex=%u)", m_buf, m_tex);
            Destroy();
            return false;
        }
        Log("[tbo] bone-palette TBO ready (buf=%u tex=%u)", m_buf, m_tex);
        return true;
    }

    void BonePaletteTBO::Begin(size_t maxBoneFloats)
    {
        // Pre-size to a generous worst-case so AppendPalette can write directly into
        // m_staging.data() (stable for the frame) after an atomic range reserve. We
        // GROW only (resize up, never shrink) so warm frames don't reallocate — and so
        // .data() does not move while workers are appending. Reset the cursor last.
        if (maxBoneFloats > m_staging.size())
            m_staging.resize(maxBoneFloats);
        m_cursor.store(0, std::memory_order_relaxed);
    }

    int BonePaletteTBO::AppendPalette(const float (*boneMatrix)[3][4], int boneCount)
    {
        if (boneMatrix == nullptr || boneCount <= 0)
            return (int)(m_cursor.load(std::memory_order_relaxed) / kFloatsPerBone);

        const size_t n   = static_cast<size_t>(boneCount) * kFloatsPerBone;
        const size_t off = m_cursor.fetch_add(n, std::memory_order_relaxed);

        // Overflow guard: the pre-sized capacity must hold the whole frame. If a frame
        // exceeds the worst case (visible-char cap underestimated), drop this palette
        // safely instead of writing out of bounds — base 0 maps to the first bone set,
        // a benign visual glitch on that one char vs. a crash. Logged once so it's not
        // silent. Begin() should size this so it never happens.
        if (off + n > m_staging.size())
        {
            static bool s_warned = false;
            if (!s_warned) { s_warned = true; Log("[tbo] palette overflow (off=%zu n=%zu cap=%zu) — raise Begin() worst-case", off, n, m_staging.size()); }
            return 0;
        }

        float* dst = m_staging.data() + off;   // .data() is stable: pre-sized in Begin()
        for (int b = 0; b < boneCount; ++b)
        {
            const float (*M)[4] = boneMatrix[b];   // [3][4] row-major affine
            // 3 texels = the 3 rows (xyz = rotation, w = translation).
            for (int r = 0; r < 3; ++r)
            {
                *dst++ = M[r][0];
                *dst++ = M[r][1];
                *dst++ = M[r][2];
                *dst++ = M[r][3];
            }
        }
        return (int)(off / kFloatsPerBone);
    }

    void BonePaletteTBO::Upload()
    {
        if (m_buf == 0 || BindBuffer == nullptr || BufferData == nullptr)
            return;
        const size_t used = m_cursor.load(std::memory_order_relaxed);   // valid length = claimed floats, NOT vector size
        if (used == 0)
            return;

        const GLsizeiptr bytes = static_cast<GLsizeiptr>(used * sizeof(float));
        BindBuffer(GL_TEXTURE_BUFFER, m_buf);
        if (bytes > m_capacityBytes)
        {
            BufferData(GL_TEXTURE_BUFFER, bytes, m_staging.data(), GL_DYNAMIC_DRAW);
            m_capacityBytes = bytes;
        }
        else if (BufferSubData != nullptr)
        {
            BufferSubData(GL_TEXTURE_BUFFER, 0, bytes, m_staging.data());
        }
        // Associate the buffer storage with the texture as RGBA32F texels.
        glBindTexture(GL_TEXTURE_BUFFER, m_tex);
        TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_buf);
        BindBuffer(GL_TEXTURE_BUFFER, 0);
    }

    void BonePaletteTBO::Bind(int unit) const
    {
        if (m_tex == 0 || ActiveTexture == nullptr)
            return;
        ActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_BUFFER, m_tex);
        ActiveTexture(GL_TEXTURE0);
    }

    BonePaletteTBO& GetBonePaletteTBO()
    {
        static BonePaletteTBO s_tbo;
        return s_tbo;
    }
}
