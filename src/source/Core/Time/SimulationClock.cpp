#include "Core/Time/SimulationClock.h"

namespace Core::Time
{
    StepResult SimulationClock::Advance(double frameMs)
    {
        if (frameMs < 0.0)              frameMs = 0.0;
        if (frameMs > m_cfg.maxFrameMs) frameMs = m_cfg.maxFrameMs;

        m_accumulator += frameMs;

        StepResult r;
        while (m_accumulator >= m_cfg.fixedDtMs && r.steps < m_cfg.maxSteps)
        {
            m_accumulator -= m_cfg.fixedDtMs;
            ++r.steps;
        }
        if (r.steps == m_cfg.maxSteps && m_accumulator >= m_cfg.fixedDtMs)
        {
            m_accumulator = 0.0;       // descartar deuda
            r.droppedDebt = true;
        }
        r.alpha = static_cast<float>(m_accumulator / m_cfg.fixedDtMs);
        return r;
    }
}
