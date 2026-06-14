#include "Core/Diagnostics/TemporalCsvLogger.h"

#include <cstdlib>

namespace Core::Diagnostics
{
    TemporalCsvLogger& TemporalCsvLogger::Instance()
    {
        static TemporalCsvLogger s_instance;
        return s_instance;
    }

    TemporalCsvLogger::TemporalCsvLogger()
    {
        char   buf[512] = {};
        size_t len = 0;
        getenv_s(&len, buf, sizeof(buf), "MU_TEMPORAL_CSV");
        if (len == 0)
            return;   // feature off by default

        m_enabled = true;

        // "1"/"on"/"true" -> default file; anything else -> treat as a path.
        const std::string value(buf);
        m_path = (value == "1" || value == "on" || value == "true")
                     ? "temporal_baseline.csv"
                     : value;
    }

    void TemporalCsvLogger::EnsureHeader()
    {
        if (m_headerWritten)
            return;

        m_out.open(m_path, std::ios::out | std::ios::trunc);
        if (m_out.is_open())
            m_out << "t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,hero_units_per_sec,steps,interp_alpha\n";

        m_headerWritten = true;
    }

    void TemporalCsvLogger::LogFrame(double timeMs, double fps, float heroX,
                                     float heroY, float heroRenderX, float heroRenderY,
                                     int steps, float alpha)
    {
        if (!m_enabled)
            return;

        EnsureHeader();
        if (!m_out.is_open())
            return;

        m_probe.Sample(heroX, heroY, timeMs);

        m_out << timeMs << ',' << fps << ',' << heroX << ',' << heroY << ','
              << heroRenderX << ',' << heroRenderY << ','
              << m_probe.UnitsPerSec() << ',' << steps << ',' << alpha << '\n';
    }
}
