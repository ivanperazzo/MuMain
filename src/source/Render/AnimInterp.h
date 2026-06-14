#pragma once

namespace Render::AnimInterp
{
    // Result of interpolating an animation frame for rendering: the fractional
    // display frame plus the prior keyframe to blend FROM. BMD::Animation blends
    // bm[priorAction][int(priorFrame)] -> bm[CurrentAction][int(frame)] by
    // frac(frame); picking the right prior keeps the pose continuous across a sim
    // tick boundary (where the engine's PriorAnimationFrame flips on an integer
    // crossing).
    struct Pose
    {
        float          frame;
        float          priorFrame;
        unsigned short priorAction;
    };

    // Maximum per-tick frame advance treated as "normal". A bigger jump means an
    // action change / reset / teleport in the animation -> snap to current instead
    // of sliding through keyframes that don't belong together.
    constexpr float kMaxTickStep = 3.0f;

    // Interpolate the body animation frame between the previous sim tick
    // (prev*) and the current values (cur*) by `alpha` (the shared frame alpha).
    // Returns the current pose unchanged when interpolation is off, on the first
    // frame (prevValid=false), on a loop wrap / no-advance (cur<=prev), or on an
    // abnormal jump (> kMaxTickStep). Pure: no engine deps -> unit-testable.
    Pose Interpolate(float prevFrame, float prevPriorFrame, unsigned short prevPriorAction,
                     float curFrame, float curPriorFrame, unsigned short curPriorAction,
                     float alpha, bool enabled, bool prevValid);
}
