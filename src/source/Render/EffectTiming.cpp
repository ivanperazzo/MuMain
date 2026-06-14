#include "Render/EffectTiming.h"

#include <algorithm>
#include <cmath>

namespace Render::EffectTiming
{
    float LinearStep(double frameMs)
    {
        if (frameMs <= 0.0)
            return 1.0f;   // no timing info -> one reference tick (original behavior)

        const double clamped = (std::min)(frameMs, kMaxFrameMs);
        return static_cast<float>(clamped / kReferenceTickMs);
    }

    float DecayPow(float base, double frameMs)
    {
        return std::pow(base, LinearStep(frameMs));
    }
}
