#include "Core/Diagnostics/MovementProbe.h"
#include <cmath>

namespace Core::Diagnostics
{
    void MovementProbe::Sample(float x, float y, double timeMs)
    {
        if (!m_hasPrev)
        {
            m_hasPrev = true;
            m_prevX = x; m_prevY = y;
            m_firstMs = m_lastMs = timeMs;
            return;
        }
        const double dx = static_cast<double>(x) - m_prevX;
        const double dy = static_cast<double>(y) - m_prevY;
        m_distance += std::sqrt(dx * dx + dy * dy);
        m_prevX = x; m_prevY = y;
        m_lastMs = timeMs;
    }

    double MovementProbe::UnitsPerSec() const
    {
        const double elapsedMs = m_lastMs - m_firstMs;
        if (elapsedMs <= 0.0) return 0.0;
        return m_distance / (elapsedMs / 1000.0);
    }

    void MovementProbe::Reset()
    {
        m_hasPrev = false;
        m_distance = 0.0;
        m_firstMs = m_lastMs = 0.0;
    }
}
