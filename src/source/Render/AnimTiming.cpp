#include "Render/AnimTiming.h"

namespace Render::AnimTiming
{
    float FrameSpeed(float baseSpeed, double frameMs)
    {
        if (frameMs <= 0.0)
            return baseSpeed;   // no timing info -> original per-frame advance

        double clamped = frameMs;
        if (clamped > kMaxFrameMs)
            clamped = kMaxFrameMs;

        return baseSpeed * static_cast<float>(clamped / kReferenceTickMs);
    }
}
