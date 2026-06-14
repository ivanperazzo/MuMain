#include "doctest.h"

#include "Render/EffectTiming.h"

#include <cmath>

using Render::EffectTiming::LinearStep;
using Render::EffectTiming::DecayPow;
using Render::EffectTiming::kReferenceTickMs;
using Render::EffectTiming::kMaxFrameMs;

namespace
{
    // Total linear advance (lifetime/timer/decay step) over `seconds` of
    // wall-clock at a fixed render FPS, summing the per-frame multiplier that an
    // effect's `x += k * EffectStep()` would apply.
    double TotalLinear(double fps, double seconds)
    {
        const double frameMs = 1000.0 / fps;
        const int    frames  = static_cast<int>((seconds * 1000.0) / frameMs + 0.5);
        double total = 0.0;
        for (int i = 0; i < frames; ++i)
            total += LinearStep(frameMs);
        return total;
    }

    // Product of an exponential decay `x *= DecayPow(base, frameMs)` over
    // `seconds` of wall-clock at a fixed render FPS.
    double TotalDecay(float base, double fps, double seconds)
    {
        const double frameMs = 1000.0 / fps;
        const int    frames  = static_cast<int>((seconds * 1000.0) / frameMs + 0.5);
        double product = 1.0;
        for (int i = 0; i < frames; ++i)
            product *= DecayPow(base, frameMs);
        return product;
    }
}

TEST_CASE("LinearStep equals one tick at the reference frame")
{
    // 40 ms frame == one sim tick -> a per-frame `+= k` advances by exactly k.
    CHECK(LinearStep(kReferenceTickMs) == doctest::Approx(1.0f));
}

TEST_CASE("Linear advance is FPS-independent (effects no longer accelerate)")
{
    // Stage 6a core: a lifetime/timer decaying at k/tick covers the same ground
    // per second whether the client renders at 30 or 240 FPS (pre-fix, at240
    // would advance 8x at30 because factor was pinned to 1.0 in MAIN_SCENE).
    const double at30  = TotalLinear(30.0,  1.0);
    const double at60  = TotalLinear(60.0,  1.0);
    const double at144 = TotalLinear(144.0, 1.0);
    const double at240 = TotalLinear(240.0, 1.0);

    const double expected = 1000.0 / kReferenceTickMs;   // 25 ticks per second
    CHECK(at30  == doctest::Approx(expected).epsilon(0.01));
    CHECK(at240 == doctest::Approx(expected).epsilon(0.01));
    CHECK(at240 == doctest::Approx(at30).epsilon(0.01));
    CHECK(at144 == doctest::Approx(at60).epsilon(0.01));
}

TEST_CASE("Exponential decay reaches the same value regardless of FPS")
{
    // `x *= pow(0.8, factor)` per frame: over one second the product must be
    // 0.8^25 at any FPS, not 0.8^FPS.
    const float base = 0.8f;
    const double reference = std::pow(static_cast<double>(base), 1000.0 / kReferenceTickMs);

    CHECK(TotalDecay(base, 30.0,  1.0) == doctest::Approx(reference).epsilon(0.02));
    CHECK(TotalDecay(base, 60.0,  1.0) == doctest::Approx(reference).epsilon(0.02));
    CHECK(TotalDecay(base, 144.0, 1.0) == doctest::Approx(reference).epsilon(0.02));
}

TEST_CASE("DecayPow returns base at the reference tick")
{
    CHECK(DecayPow(0.8f, kReferenceTickMs) == doctest::Approx(0.8f));
    CHECK(DecayPow(0.5f, kReferenceTickMs) == doctest::Approx(0.5f));
}

TEST_CASE("Non-positive frameMs falls back to one reference tick")
{
    // Outside MAIN_SCENE / before any frame time is known, behave as the original
    // single-tick step so login/menu effects are unchanged.
    CHECK(LinearStep(0.0)  == doctest::Approx(1.0f));
    CHECK(LinearStep(-5.0) == doctest::Approx(1.0f));
    CHECK(DecayPow(0.8f, 0.0) == doctest::Approx(0.8f));
}

TEST_CASE("A stalled frame is clamped, not flung forward")
{
    // A 1 s hitch must not expire an effect by 25 ticks at once.
    const double clamped = LinearStep(1000.0);
    const double cap     = kMaxFrameMs / kReferenceTickMs;
    CHECK(clamped == doctest::Approx(cap));
    CHECK(clamped < 1000.0 / kReferenceTickMs);   // less than un-clamped
}
