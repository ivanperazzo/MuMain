#pragma once

namespace Render::AnimTiming
{
    // The sim runs one fixed tick every 40 ms (25 tps = REFERENCE_FPS). Animation
    // that advances in the SIM tick adds `PlaySpeed` once per tick -> PlaySpeed*25
    // frames/sec, FPS-independent. Animation that advances in the RENDER path
    // (attached parts: wings/weapons/capes in RenderLinkObject) instead adds
    // PlaySpeed once per *render frame*, so after Stage 1b pinned
    // FPS_ANIMATION_FACTOR=1.0 those parts speed up with FPS.
    //
    // FrameSpeed rescales a per-frame PlaySpeed by the real frame duration so the
    // total advance per second equals the sim rate (PlaySpeed*25), independent of
    // how many frames render in that second. Pure, no engine deps -> unit-testable.
    constexpr double kReferenceTickMs = 40.0;   // 25 tps sim tick
    constexpr double kMaxFrameMs      = 250.0;  // stall clamp (matches SimulationClock)

    // Returns baseSpeed * (frameMs / 40), clamped so a stalled frame can't fling
    // the animation forward. frameMs <= 0 (no timing info, e.g. menu scenes) ->
    // returns baseSpeed unchanged, preserving original per-frame behavior.
    float FrameSpeed(float baseSpeed, double frameMs);
}
