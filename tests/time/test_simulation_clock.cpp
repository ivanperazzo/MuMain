#include "doctest.h"

#include "Core/Time/SimulationClock.h"

using Core::Time::SimulationClock;
using Core::Time::ClockConfig;

TEST_CASE("one fixed frame yields one step, zero alpha")
{
    SimulationClock clk;                 // default 40ms / maxSteps 5
    auto r = clk.Advance(40.0);
    CHECK(r.steps == 1);
    CHECK(r.alpha == doctest::Approx(0.0f));
    CHECK(r.droppedDebt == false);
}

TEST_CASE("partial frame accumulates into alpha")
{
    SimulationClock clk;
    auto r = clk.Advance(20.0);          // half a tick
    CHECK(r.steps == 0);
    CHECK(r.alpha == doctest::Approx(0.5f));
}

TEST_CASE("100ms frame: 2 steps, 20ms leftover, alpha 0.5")
{
    SimulationClock clk;
    auto r = clk.Advance(100.0);
    CHECK(r.steps == 2);
    CHECK(r.alpha == doctest::Approx(0.5f));
}

TEST_CASE("step count is frame-rate independent over the same wall time")
{
    SimulationClock fast;
    int fastSteps = 0;
    for (int i = 0; i < 125; ++i) fastSteps += fast.Advance(8.0).steps;   // ~125 FPS

    SimulationClock slow;
    int slowSteps = 0;
    for (int i = 0; i < 25; ++i) slowSteps += slow.Advance(40.0).steps;   // 25 FPS

    CHECK(fastSteps == 25);
    CHECK(slowSteps == 25);
}

TEST_CASE("spiral of death is cut by MAX_STEPS")
{
    SimulationClock clk;                 // maxSteps 5, maxFrameMs 250
    auto r = clk.Advance(1000.0);        // clamped to 250 -> 6.25 ticks -> 5 + debt
    CHECK(r.steps == 5);
    CHECK(r.droppedDebt == true);
    CHECK(clk.Accumulator() == doctest::Approx(0.0));
}

TEST_CASE("negative frameMs is guarded")
{
    SimulationClock clk;
    auto r = clk.Advance(-5.0);
    CHECK(r.steps == 0);
    CHECK(r.alpha == doctest::Approx(0.0f));
}
