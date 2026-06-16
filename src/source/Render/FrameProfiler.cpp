#include "Render/FrameProfiler.h"

#include <chrono>

namespace Render::FrameProfiler
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        Clock::time_point s_renderStart{};
        Clock::time_point s_swapStart{};
        double            s_lastCpuRenderMs = 0.0;
        double            s_lastSwapMs      = 0.0;

        double MsSince(Clock::time_point start)
        {
            const auto d = Clock::now() - start;
            return std::chrono::duration<double, std::milli>(d).count();
        }
    }

    void   BeginRender()        { s_renderStart = Clock::now(); }
    void   EndRender()          { s_lastCpuRenderMs = MsSince(s_renderStart); }
    double LastCpuRenderMs()    { return s_lastCpuRenderMs; }

    void   BeginSwap()          { s_swapStart = Clock::now(); }
    void   EndSwap()            { s_lastSwapMs = MsSince(s_swapStart); }
    double LastSwapMs()         { return s_lastSwapMs; }
}
