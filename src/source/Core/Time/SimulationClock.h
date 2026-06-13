#pragma once

namespace Core::Time
{
    struct ClockConfig
    {
        double fixedDtMs  = 40.0;    // 25 tps — invariante de balance (= REFERENCE_FPS actual)
        int    maxSteps   = 5;       // corta la spiral of death
        double maxFrameMs = 250.0;   // clamp de stall antes de acumular
    };

    struct StepResult
    {
        int   steps       = 0;       // cuantos FixedUpdate ejecutar este frame
        float alpha       = 0.0f;    // interpolacion de render [0,1)
        bool  droppedDebt = false;   // true si se descarto deuda por maxSteps
    };

    // Pure fixed-timestep accumulator. No engine deps -> unit-testable.
    // Feed the wall-clock frame delta (ms); get back how many fixed sim steps to
    // run this frame and the render interpolation alpha.
    class SimulationClock
    {
    public:
        explicit SimulationClock(ClockConfig cfg = {}) : m_cfg(cfg) {}

        StepResult Advance(double frameMs);

        double FixedDtMs()   const { return m_cfg.fixedDtMs; }
        double Accumulator() const { return m_accumulator; }
        void   Reset()             { m_accumulator = 0.0; }

    private:
        ClockConfig m_cfg;
        double      m_accumulator = 0.0;
    };
}
