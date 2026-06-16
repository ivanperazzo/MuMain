#include "Render/HeroInterpolation.h"

#include "Render/Interpolation.h"

namespace Render::HeroInterp
{
    namespace
    {
        float s_prev[3]   = {0.f, 0.f, 0.f};
        bool  s_hasPrev   = false;
    }

    void OnTick(const float curPos[3])
    {
        s_prev[0] = curPos[0];
        s_prev[1] = curPos[1];
        s_prev[2] = curPos[2];
        s_hasPrev = true;
    }

    void RenderPos(const float curPos[3], float out[3])
    {
        if (!s_hasPrev)
        {
            out[0] = curPos[0]; out[1] = curPos[1]; out[2] = curPos[2];
            return;
        }

        Render::Interpolation::LerpGuarded(
            s_prev, curPos, Render::Interpolation::FrameAlpha(), out);
    }
}
