#pragma once

namespace Core::Diagnostics
{
    // Pure rolling probe: feed (x, y, timeMs) samples, get distance/sec over the
    // window. No engine deps -> unit-testable. Frame-rate independent because it
    // divides accumulated distance by accumulated wall time, not by frame count.
    class MovementProbe
    {
    public:
        void  Sample(float x, float y, double timeMs);
        double UnitsPerSec() const;   // 0 until at least two samples exist
        void  Reset();

    private:
        bool   m_hasPrev = false;
        float  m_prevX = 0.0f, m_prevY = 0.0f;
        double m_firstMs = 0.0, m_lastMs = 0.0;
        double m_distance = 0.0;
    };
}
