#include "doctest.h"

#include "Render/AnimTiming.h"

using Render::AnimTiming::FrameSpeed;
using Render::AnimTiming::kReferenceTickMs;
using Render::AnimTiming::kMaxFrameMs;

namespace
{
    // Total animation advance over `seconds` of wall-clock at a fixed render FPS,
    // summing the per-frame delta that RenderLinkObject would apply.
    double TotalAdvance(float baseSpeed, double fps, double seconds)
    {
        const double frameMs = 1000.0 / fps;
        const int    frames  = static_cast<int>((seconds * 1000.0) / frameMs + 0.5);
        double total = 0.0;
        for (int i = 0; i < frames; ++i)
            total += FrameSpeed(baseSpeed, frameMs);
        return total;
    }
}

TEST_CASE("FrameSpeed equals base speed at the reference tick")
{
    // 40 ms frame == one sim tick -> render-path advance matches sim-path advance.
    CHECK(FrameSpeed(0.5f, kReferenceTickMs) == doctest::Approx(0.5f));
    CHECK(FrameSpeed(1.0f, kReferenceTickMs) == doctest::Approx(1.0f));
}

TEST_CASE("Total advance is FPS-independent (no double speed at high FPS)")
{
    // The whole point of Stage 4a: a part animated at PlaySpeed advances the same
    // number of frames per second whether the client renders at 30 or 240 FPS.
    const float base = 0.5f;
    const double at30  = TotalAdvance(base, 30.0,  1.0);
    const double at60  = TotalAdvance(base, 60.0,  1.0);
    const double at144 = TotalAdvance(base, 144.0, 1.0);
    const double at240 = TotalAdvance(base, 240.0, 1.0);

    // Reference: base * (1000ms / 40ms) = base * 25 frames per second.
    const double expected = base * (1000.0 / kReferenceTickMs);
    CHECK(at30  == doctest::Approx(expected).epsilon(0.01));
    CHECK(at240 == doctest::Approx(expected).epsilon(0.01));

    // And they agree with each other (the regression we are guarding against:
    // pre-fix, at240 would be 8x at30).
    CHECK(at240 == doctest::Approx(at30).epsilon(0.01));
    CHECK(at144 == doctest::Approx(at60).epsilon(0.01));
}

TEST_CASE("Non-positive frameMs falls back to the raw per-frame speed")
{
    // Menu/character-select scenes have no fixed-step timing -> preserve original
    // behavior (PlayAnimation still applies its own FPS_ANIMATION_FACTOR there).
    CHECK(FrameSpeed(0.7f, 0.0)  == doctest::Approx(0.7f));
    CHECK(FrameSpeed(0.7f, -5.0) == doctest::Approx(0.7f));
}

TEST_CASE("A stalled frame is clamped, not flung forward")
{
    // A 1 s hitch must not advance the animation by 25 frames at once.
    const float base = 0.5f;
    const double clamped = FrameSpeed(base, 1000.0);
    const double cap     = base * (kMaxFrameMs / kReferenceTickMs);
    CHECK(clamped == doctest::Approx(cap));
    CHECK(clamped < base * (1000.0 / kReferenceTickMs));   // less than un-clamped
}

TEST_CASE("Zero play speed never advances")
{
    CHECK(FrameSpeed(0.0f, 16.6) == doctest::Approx(0.0f));
    CHECK(FrameSpeed(0.0f, kReferenceTickMs) == doctest::Approx(0.0f));
}
