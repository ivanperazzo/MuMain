#include "Motion/Integrate.h"

namespace Motion
{
    namespace
    {
        constexpr double kReferenceTickMs = 40.0;   // 25 tps, matches SimulationClock
    }

    void IntegratePosition(const float pos[3], const float vel[3], double dtMs, float out[3])
    {
        const float scale = static_cast<float>(dtMs / kReferenceTickMs);
        for (int i = 0; i < 3; ++i)
            out[i] = pos[i] + vel[i] * scale;
    }
}
