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

    int BonePaletteTBO::AppendPalette(const float (*boneMatrix)[3][4], int boneCount)
    {
        const int base = BoneCount();
        if (boneMatrix == nullptr || boneCount <= 0)
            return base;

        m_staging.reserve(m_staging.size() + static_cast<size_t>(boneCount) * kFloatsPerBone);
        for (int b = 0; b < boneCount; ++b)
        {
            const float (*M)[4] = boneMatrix[b];   // [3][4] row-major affine
            // 3 texels = the 3 rows (xyz = rotation, w = translation).
            for (int r = 0; r < 3; ++r)
            {
                m_staging.push_back(M[r][0]);
                m_staging.push_back(M[r][1]);
                m_staging.push_back(M[r][2]);
                m_staging.push_back(M[r][3]);
            }
        }
        return base;
    }

    void BonePaletteTBO::Upload()
    {
        if (m_buf == 0 || BindBuffer == nullptr || BufferData == nullptr)
            return;
        if (m_staging.empty())
            return;

        const GLsizeiptr bytes = static_cast<GLsizeiptr>(m_staging.size() * sizeof(float));
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
