#include "Render/AnimInterp.h"

namespace Render::AnimInterp
{
    Pose Interpolate(float prevFrame, float prevPriorFrame, unsigned short prevPriorAction,
                     float curFrame, float curPriorFrame, unsigned short curPriorAction,
                     float alpha, bool enabled, bool prevValid)
    {
        const Pose cur{curFrame, curPriorFrame, curPriorAction};

        if (!enabled || !prevValid)
            return cur;

        const float d = curFrame - prevFrame;
        if (d <= 0.f || d > kMaxTickStep)   // wrap/reset/no-advance or abnormal jump
            return cur;

        if (alpha < 0.f) alpha = 0.f;
        if (alpha > 1.f) alpha = 1.f;

        const float display = prevFrame + d * alpha;

        // Pick the prior keyframe matching which integer interval `display` sits
        // in. `display` lies in [prevFrame, curFrame], so its integer part is
        // either int(prevFrame) or int(curFrame). When it has reached the current
        // tick's integer, use the post-advance prior; otherwise the pre-advance
        // one. This keeps the blend continuous at the tick boundary.
        if (static_cast<int>(display) >= static_cast<int>(curFrame))
            return Pose{display, curPriorFrame, curPriorAction};

        return Pose{display, prevPriorFrame, prevPriorAction};
    }
}
