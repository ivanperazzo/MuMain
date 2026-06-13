#include "doctest.h"

#include <cmath>

#include "Core/Time/SimulationClock.h"

using Core::Time::SimulationClock;

// Models how RenderScene drives the fixed step in Stage 1b: Advance() is called
// once per RENDER frame with the wall time elapsed since the previous render,
// and that many MainSceneFixedUpdate ticks run. The total tick count must track
// wall_time / 40ms regardless of how fast or jittery the render rate is — that
// invariance is what kills the FPS-coupled speedhack.

TEST_CASE("fixed-step driver runs ~25 ticks/sec over jittery render frames")
{
    SimulationClock clk;
    int    totalSteps = 0;
    double wallMs = 0.0;

    // Deterministic jitter: render frame times bouncing between ~16 and ~250 FPS.
    const double frameMsTable[] = { 6.0, 7.0, 33.0, 8.0, 16.0, 60.0, 4.0, 11.0, 40.0, 9.0 };
    const int    n = sizeof(frameMsTable) / sizeof(frameMsTable[0]);

    for (int i = 0; wallMs < 10000.0; ++i)
    {
        const double f = frameMsTable[i % n];
        wallMs += f;
        totalSteps += clk.Advance(f).steps;
    }

    const double expected = wallMs / 40.0;   // 40 ms = 25 tps
    // Off by at most the sub-tick remainder still in the accumulator (< 1 tick).
    CHECK(std::abs(static_cast<double>(totalSteps) - expected) <= 1.5);
}

TEST_CASE("a long render stall does not burst-advance afterward")
{
    SimulationClock clk;                 // maxSteps 5, maxFrameMs 250
    for (int i = 0; i < 10; ++i)
        clk.Advance(40.0);               // steady state

    const auto stall = clk.Advance(2000.0);   // clamped to 250 -> 6.25 ticks
    CHECK(stall.steps <= 5);
    CHECK(stall.droppedDebt == true);

    // Debt was dropped (accumulator reset), so the next normal frame yields
    // exactly one tick — no accumulated catch-up burst.
    const auto after = clk.Advance(40.0);
    CHECK(after.steps == 1);
    CHECK(after.droppedDebt == false);
}
