#include "doctest.h"

#include "Render/Interpolation.h"
#include "Motion/Integrate.h"

TEST_CASE("Lerp: endpoints and midpoint, per component")
{
    const float a[3] = {0.f, 10.f, -4.f};
    const float b[3] = {10.f, 30.f, 4.f};
    float out[3];

    Render::Interpolation::Lerp(a, b, 0.0f, out);
    CHECK(out[0] == doctest::Approx(0.f));
    CHECK(out[1] == doctest::Approx(10.f));
    CHECK(out[2] == doctest::Approx(-4.f));

    Render::Interpolation::Lerp(a, b, 1.0f, out);
    CHECK(out[0] == doctest::Approx(10.f));
    CHECK(out[1] == doctest::Approx(30.f));
    CHECK(out[2] == doctest::Approx(4.f));

    Render::Interpolation::Lerp(a, b, 0.5f, out);
    CHECK(out[0] == doctest::Approx(5.f));
    CHECK(out[1] == doctest::Approx(20.f));
    CHECK(out[2] == doctest::Approx(0.f));
}

TEST_CASE("Lerp clamps alpha to [0,1]")
{
    const float a[3] = {0.f, 0.f, 0.f};
    const float b[3] = {10.f, 0.f, 0.f};
    float out[3];

    Render::Interpolation::Lerp(a, b, -1.0f, out);
    CHECK(out[0] == doctest::Approx(0.f));    // clamped to a

    Render::Interpolation::Lerp(a, b, 2.0f, out);
    CHECK(out[0] == doctest::Approx(10.f));   // clamped to b
}

TEST_CASE("IntegratePosition: one fixed tick adds full velocity")
{
    const float pos[3] = {100.f, 0.f, 0.f};
    const float vel[3] = {0.f, -8.f, 0.f};
    float out[3];

    Motion::IntegratePosition(pos, vel, 40.0, out);   // dt = one reference tick
    CHECK(out[0] == doctest::Approx(100.f));
    CHECK(out[1] == doctest::Approx(-8.f));
}

TEST_CASE("IntegratePosition is frame-rate invariant over equal wall time")
{
    const float vel[3] = {4.f, 0.f, 0.f};

    float coarse[3] = {0.f, 0.f, 0.f};
    Motion::IntegratePosition(coarse, vel, 40.0, coarse);          // 1 x 40ms

    float fine[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 5; ++i)
        Motion::IntegratePosition(fine, vel, 8.0, fine);          // 5 x 8ms

    CHECK(coarse[0] == doctest::Approx(fine[0]));   // same total displacement
}
