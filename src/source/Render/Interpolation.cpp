#include "Render/Interpolation.h"

namespace Render::Interpolation
{
    namespace
    {
        // Must be >= engine MAX_CHARACTERS_CLIENT (Core/Globals/_define.h = 400).
        // Kept as a local constant so this TU stays free of engine headers and
        // remains unit-testable; the index guard below makes a mismatch graceful
        // (out-of-range slots just render un-interpolated instead of crashing).
        constexpr int kMaxRemoteSlots = 400;

        bool  s_enabled = true;
        float s_frameAlpha = 0.f;
        float s_remotePrev[kMaxRemoteSlots][3] = {};

        // A single 25 tps tick of walking moves only a few units; > ~3 tiles in
        // one tick is a teleport/warp/spawn -> snap instead of sliding across it.
        constexpr float kTeleportDistSq = 300.f * 300.f;
    }

    void Lerp(const float a[3], const float b[3], float alpha, float out[3])
    {
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        for (int i = 0; i < 3; ++i)
            out[i] = a[i] + (b[i] - a[i]) * alpha;
    }

    void LerpGuarded(const float prev[3], const float cur[3], float alpha, float out[3])
    {
        if (!s_enabled)   // $interp off -> render the raw 25 Hz sim position
        {
            out[0] = cur[0]; out[1] = cur[1]; out[2] = cur[2];
            return;
        }

        const float dx = cur[0] - prev[0];
        const float dy = cur[1] - prev[1];
        if (dx * dx + dy * dy > kTeleportDistSq)
        {
            out[0] = cur[0]; out[1] = cur[1]; out[2] = cur[2];
            return;
        }
        Lerp(prev, cur, alpha, out);
    }

    void SetEnabled(bool on) { s_enabled = on; }
    bool Enabled()           { return s_enabled; }

    void SetFrameAlpha(float alpha) { s_frameAlpha = alpha; }
    float FrameAlpha()              { return s_frameAlpha; }

    void RemoteOnTick(int index, const float curPos[3])
    {
        if (index < 0 || index >= kMaxRemoteSlots)
            return;
        s_remotePrev[index][0] = curPos[0];
        s_remotePrev[index][1] = curPos[1];
        s_remotePrev[index][2] = curPos[2];
    }

    void RemoteRenderPos(int index, const float curPos[3], float out[3])
    {
        if (index < 0 || index >= kMaxRemoteSlots)
        {
            out[0] = curPos[0]; out[1] = curPos[1]; out[2] = curPos[2];
            return;
        }
        LerpGuarded(s_remotePrev[index], curPos, s_frameAlpha, out);
    }
}
