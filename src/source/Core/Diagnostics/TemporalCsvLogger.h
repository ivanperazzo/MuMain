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

        // Record one render frame. heroX/heroY = raw sim position; heroRenderX/Y =
        // interpolated render position (Stage 2). At high FPS the rendered position
        // moves in small smooth steps while the raw one jumps at 25 Hz.
        // frameMs = real frame duration (Stage 4): lets the offline analyzer
        // recompute render-path animation advance rate (old = frames/s scales with
        // FPS; new = sum(frameMs/40) stays ~25/s, the fix).
        // animRaw/animRender = Hero body animation frame, raw sim value vs the
        // interpolated render value (Stage 4b): raw steps at 25 Hz, render advances
        // its fraction smoothly every frame.
        // effStep/effDecay = the real Render::EffectTiming glue sampled this frame
        // (Stage 6a): effStep is the per-frame linear step (`x -= k*effStep`), it
        // must sum to ~25/s at any FPS; effDecay is the per-frame exp-decay factor
        // pow(0.8, dt). These prove the dt-substitution happens at runtime in
        // MAIN_SCENE, not just in the unit test.
        // cpuRenderMs/swapMs = P0 GPU-track split (prev frame): CPU draw build vs
        // present. CPU-bound -> cpuRenderMs dominates, swapMs ~0; GPU/present-bound
        // -> swapMs large (VSync wait or GPU catch-up).
        void LogFrame(double timeMs, double fps, float heroX, float heroY,
                      float heroRenderX, float heroRenderY, int steps, float alpha,
                      double frameMs, float animRaw, float animRender,
                      float effStep, float effDecay,
                      double cpuRenderMs, double swapMs);

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
