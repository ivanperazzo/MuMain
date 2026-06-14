#pragma once

namespace Render::Interpolation
{
    // Component-wise linear interpolation from a to b by alpha (clamped to
    // [0,1]). Pure, no engine deps -> unit-testable. Used to render an entity
    // between two fixed simulation ticks: a = previous tick position, b =
    // current tick position, alpha = SimulationClock render interpolation factor.
    // out may alias a or b.
    void Lerp(const float a[3], const float b[3], float alpha, float out[3]);
}
