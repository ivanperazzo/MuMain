// Scene-aware glue for Render::EffectTiming. Kept in its own TU so the pure math
// (EffectTiming.cpp) stays free of engine globals and unit-testable. Stage 6a.
//
// Inside MAIN_SCENE the per-render-frame `* FPS_ANIMATION_FACTOR` increments are
// FPS-coupled (1b pinned that factor to 1.0 there), so effects/timers/decays run
// ~FPS/25 times too fast. Here we substitute a real-time step (frameMs/40). In
// every other scene (login/menu) the factor is still the REFERENCE_FPS/FPS clamp
// and must be preserved untouched, so we fall back to FPS_ANIMATION_FACTOR.

#include "stdafx.h"                  // PCH: windows.h + EGameScene/MAIN_SCENE (_define.h)
#include "Render/EffectTiming.h"
#include "Render/Interpolation.h"

#include <cmath>

extern EGameScene SceneFlag;            // Scenes/SceneCore.h
extern float      FPS_ANIMATION_FACTOR; // Engine/AI/ZzzAI.cpp

namespace Render::EffectTiming
{
    namespace
    {
        // Real frame time is only meaningful in MAIN_SCENE (where the factor was
        // pinned to 1.0). Elsewhere keep the original frame-rate compensation.
        bool UseRealTime()
        {
            return SceneFlag == MAIN_SCENE && Interpolation::FrameMs() > 0.0;
        }
    }

    float EffectStep()
    {
        if (UseRealTime())
            return LinearStep(Interpolation::FrameMs());
        return FPS_ANIMATION_FACTOR;
    }

    float EffectDecayExp(float base)
    {
        if (UseRealTime())
            return DecayPow(base, Interpolation::FrameMs());
        return std::pow(base, FPS_ANIMATION_FACTOR);
    }
}
