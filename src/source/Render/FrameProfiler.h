#pragma once

namespace Render::FrameProfiler
{
    // P0 instrumentation for the GPU/high-FPS track. Splits a render frame into
    // CPU build/submit time vs present (swap) time, so a baseline capture can tell
    // whether the client is CPU-bound (cpu_render_ms dominates, swap ~0) or
    // GPU/present-bound (swap_ms large, e.g. VSync wait or GPU catch-up).
    //
    // Pure timing (std::chrono::steady_clock), no engine deps. Begin/End are
    // called around the scene draw and the buffer swap in MainScene(); the CSV
    // logger reads the Last*Ms() accessors (one frame of lag, fine for averaging).
    void   BeginRender();
    void   EndRender();
    double LastCpuRenderMs();

    void   BeginSwap();
    void   EndSwap();
    double LastSwapMs();
}
