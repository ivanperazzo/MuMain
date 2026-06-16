#pragma once

namespace Motion
{
    // Advances position by velocity over dtMs, where velocity is expressed in
    // units per reference tick (40 ms = 25 tps). At dtMs == 40 this is simply
    // pos += vel (one tick). Pure, no engine deps -> unit-testable; out may
    // alias pos. out = pos + vel * (dtMs / 40).
    void IntegratePosition(const float pos[3], const float vel[3], double dtMs, float out[3]);
}
