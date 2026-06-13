#pragma once

#include <fstream>
#include <string>

#include "Core/Diagnostics/MovementProbe.h"

namespace Core::Diagnostics
{
    // Stage 0 instrumentation for the temporal-decoupling work. Records one CSV
    // row per render frame so the speed of the game can be measured at different
    // frame rates (the baseline every later stage is verified against).
    //
    // OFF by default: enabled only when the env var MU_TEMPORAL_CSV is set. When
    // disabled, Enabled() is a single bool read, so the per-frame call site costs
    // nothing in a normal build (no I/O, no allocation on the hot path).
    //   MU_TEMPORAL_CSV=1                  -> writes ./temporal_baseline.csv
    //   MU_TEMPORAL_CSV=C:\path\run.csv    -> writes that file
    //
    // Owns a MovementProbe (the unit-tested speed math). The raw position columns
    // also let the true moving speed be recomputed offline, independent of the
    // probe's rolling window.
    class TemporalCsvLogger
    {
    public:
        static TemporalCsvLogger& Instance();

        bool Enabled() const { return m_enabled; }

        // Record one render frame. `steps` and `alpha` stay 0 until Stage 1b
        // wires the SimulationClock into the main loop.
        void LogFrame(double timeMs, double fps, float heroX, float heroY,
                      int steps, float alpha);

    private:
        TemporalCsvLogger();   // reads the env var once
        void EnsureHeader();

        bool          m_enabled = false;
        bool          m_headerWritten = false;
        std::string   m_path;
        std::ofstream m_out;
        MovementProbe m_probe;
    };
}
