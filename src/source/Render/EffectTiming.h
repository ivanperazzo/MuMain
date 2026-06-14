#pragma once

namespace Render::EffectTiming
{
    // Stage 6: effect/particle/physics code advances state once per RENDER frame
    // scaled by FPS_ANIMATION_FACTOR. After 1b pinned that factor to 1.0 in
    // MAIN_SCENE, those increments run ~FPS/25 times too fast (effects accelerate,
    // fade quicker, scatter more at high FPS). The fix is to scale by real frame
    // time instead: dt = frameMs / 40 (40 ms = the 25 tps reference tick), which is
    // exactly what REFERENCE_FPS/FPS approximated but using the actual frame length.
    //
    // Pure math (unit-tested). The scene-aware accessors below pick dt in MAIN_SCENE
    // and fall back to the original FPS_ANIMATION_FACTOR elsewhere (login/menu still
    // use the clamp and must keep working) -- those live in EffectTimingGlue.cpp so
    // this TU stays free of engine globals.
    constexpr double kReferenceTickMs = 40.0;   // 25 tps
    constexpr double kMaxFrameMs      = 250.0;  // stall clamp (matches SimulationClock)

    // Per-frame step multiplier from real frame time: clamp(frameMs)/40. Replaces a
    // raw `* FPS_ANIMATION_FACTOR` for linear advance/decay/timers. frameMs <= 0 ->
    // 1.0 (one reference tick), a safe default.
    float LinearStep(double frameMs);

    // Frame-rate-independent exponential decay factor: pow(base, LinearStep(frameMs)).
    // Replaces `pow(base, FPS_ANIMATION_FACTOR)`. At the 40 ms reference returns base;
    // the product over one second is base^25 at any FPS.
    float DecayPow(float base, double frameMs);

    // --- scene-aware glue (EffectTimingGlue.cpp) ---
    // MAIN_SCENE with a known frame time -> dt-based; otherwise the original
    // FPS_ANIMATION_FACTOR (so non-MAIN scenes are unchanged).
    float EffectStep();               // replaces `* FPS_ANIMATION_FACTOR`
    float EffectDecayExp(float base);  // replaces `pow(base, FPS_ANIMATION_FACTOR)`
}
