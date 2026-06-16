#include "doctest.h"

#include "Render/AnimInterp.h"

using Render::AnimInterp::Interpolate;
using Render::AnimInterp::Pose;
using Render::AnimInterp::kMaxTickStep;

// Args: prevFrame, prevPriorFrame, prevPriorAction, curFrame, curPriorFrame,
//       curPriorAction, alpha, enabled, prevValid

TEST_CASE("Disabled or first frame returns the current pose unchanged")
{
    Pose off = Interpolate(3.0f, 2.f, 1, 3.5f, 2.f, 1, 0.5f, /*enabled*/false, true);
    CHECK(off.frame == doctest::Approx(3.5f));

    Pose first = Interpolate(3.0f, 2.f, 1, 3.5f, 2.f, 1, 0.5f, true, /*prevValid*/false);
    CHECK(first.frame == doctest::Approx(3.5f));
}

TEST_CASE("Within a keyframe interval, the display frame advances smoothly")
{
    // No integer crossing this tick: prev=3.2 -> cur=3.6, prior constant.
    Pose a = Interpolate(3.2f, 2.f, 1, 3.6f, 2.f, 1, 0.0f, true, true);
    Pose b = Interpolate(3.2f, 2.f, 1, 3.6f, 2.f, 1, 0.5f, true, true);
    Pose c = Interpolate(3.2f, 2.f, 1, 3.6f, 2.f, 1, 1.0f, true, true);
    CHECK(a.frame == doctest::Approx(3.2f));
    CHECK(b.frame == doctest::Approx(3.4f));
    CHECK(c.frame == doctest::Approx(3.6f));
    // monotonic and strictly between prev and cur for mid alpha
    CHECK(b.frame > a.frame);
    CHECK(b.frame < c.frame);
}

TEST_CASE("Prior keyframe is selected to stay continuous across a tick boundary")
{
    // Crossing tick: prev=3.8 (prior 2), cur=4.3 (prior 3, set on crossing 3->4).
    Pose lo  = Interpolate(3.8f, 2.f, 7, 4.3f, 3.f, 9, 0.0f, true, true);  // display 3.8
    Pose mid = Interpolate(3.8f, 2.f, 7, 4.3f, 3.f, 9, 0.6f, true, true);  // display 4.1
    Pose hi  = Interpolate(3.8f, 2.f, 7, 4.3f, 3.f, 9, 1.0f, true, true);  // display 4.3

    // Before reaching the new integer -> pre-advance prior (frame 2, action 7).
    CHECK((int)lo.frame == 3);
    CHECK(lo.priorFrame == doctest::Approx(2.f));
    CHECK(lo.priorAction == 7);

    // After reaching it -> post-advance prior (frame 3, action 9).
    CHECK((int)mid.frame == 4);
    CHECK(mid.priorFrame == doctest::Approx(3.f));
    CHECK(mid.priorAction == 9);
    CHECK((int)hi.frame == 4);
    CHECK(hi.priorAction == 9);
}

TEST_CASE("A loop wrap (cur < prev) snaps to current, never plays backwards")
{
    Pose w = Interpolate(9.9f, 8.f, 1, 0.1f, 0.f, 1, 0.5f, true, true);
    CHECK(w.frame == doctest::Approx(0.1f));   // no backward slide
}

TEST_CASE("An abnormal jump (action change / reset) snaps to current")
{
    Pose j = Interpolate(1.0f, 0.f, 1, 1.0f + kMaxTickStep + 1.0f, 0.f, 2, 0.5f, true, true);
    CHECK(j.frame == doctest::Approx(1.0f + kMaxTickStep + 1.0f));
}

TEST_CASE("No advance (cur == prev) returns current, no interpolation")
{
    Pose s = Interpolate(5.0f, 4.f, 1, 5.0f, 4.f, 1, 0.5f, true, true);
    CHECK(s.frame == doctest::Approx(5.0f));
}

TEST_CASE("Alpha is clamped to [0,1]")
{
    Pose under = Interpolate(2.0f, 1.f, 1, 2.4f, 1.f, 1, -1.0f, true, true);
    Pose over  = Interpolate(2.0f, 1.f, 1, 2.4f, 1.f, 1,  2.0f, true, true);
    CHECK(under.frame == doctest::Approx(2.0f));
    CHECK(over.frame  == doctest::Approx(2.4f));
}
