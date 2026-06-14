#include "Render/HeroInterpolation.h"

#include "Render/Interpolation.h"

namespace Render::HeroInterp
{
    namespace
    {
        float s_prev[3] = {0.f, 0.f, 0.f};
        float s_alpha   = 0.f;
        bool  s_hasPrev = false;

        // A single 25 tps tick of walking moves only a few units; > ~3 tiles in
        // one tick is a teleport/warp/spawn -> snap instead of sliding across it.
        constexpr float kTeleportDistSq = 300.f * 300.f;
    }

    void OnTick(const float curPos[3])
    {
        s_prev[0] = curPos[0];
        s_prev[1] = curPos[1];
        s_prev[2] = curPos[2];
        s_hasPrev = true;
    }

    void SetAlpha(float alpha)
    {
        s_alpha = alpha;
    }

    void RenderPos(const float curPos[3], float out[3])
    {
        if (!s_hasPrev)
        {
            out[0] = curPos[0]; out[1] = curPos[1]; out[2] = curPos[2];
            return;
        }

        const float dx = curPos[0] - s_prev[0];
        const float dy = curPos[1] - s_prev[1];
        if (dx * dx + dy * dy > kTeleportDistSq)
        {
            out[0] = curPos[0]; out[1] = curPos[1]; out[2] = curPos[2];
            return;
        }

        Render::Interpolation::Lerp(s_prev, curPos, s_alpha, out);
    }
}
