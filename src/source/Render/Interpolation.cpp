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

        bool   s_enabled = true;
        bool   s_poseEnabled = true;    // Stage 4b body-pose interp (validated); $poseinterp off to compare
        float  s_frameAlpha = 0.f;
        double s_frameMs = 0.0;
        float  s_remotePrev[kMaxRemoteSlots][3] = {};

        // Stage 4b: per-slot prev-tick body animation state (parallel to OBJECT).
        float          s_remoteAnimFrame[kMaxRemoteSlots] = {};
        float          s_remoteAnimPriorFrame[kMaxRemoteSlots] = {};
        unsigned short s_remoteAnimPriorAction[kMaxRemoteSlots] = {};
        bool           s_remoteAnimValid[kMaxRemoteSlots] = {};

        // Hero prev-tick body animation state.
        float          s_heroAnimFrame = 0.f;
        float          s_heroAnimPriorFrame = 0.f;
        unsigned short s_heroAnimPriorAction = 0;
        bool           s_heroAnimValid = false;

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

    void SetPoseEnabled(bool on) { s_poseEnabled = on; }
    bool PoseEnabled()           { return s_poseEnabled; }

    void SetFrameAlpha(float alpha) { s_frameAlpha = alpha; }
    float FrameAlpha()              { return s_frameAlpha; }

    void SetFrameMs(double ms) { s_frameMs = ms; }
    double FrameMs()           { return s_frameMs; }

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

    void RemoteAnimOnTick(int index, float frame, float priorFrame, unsigned short priorAction)
    {
        if (index < 0 || index >= kMaxRemoteSlots)
            return;
        s_remoteAnimFrame[index]       = frame;
        s_remoteAnimPriorFrame[index]  = priorFrame;
        s_remoteAnimPriorAction[index] = priorAction;
        s_remoteAnimValid[index]       = true;
    }

    bool RemoteAnimPrev(int index, float& frame, float& priorFrame, unsigned short& priorAction)
    {
        if (index < 0 || index >= kMaxRemoteSlots || !s_remoteAnimValid[index])
            return false;
        frame       = s_remoteAnimFrame[index];
        priorFrame  = s_remoteAnimPriorFrame[index];
        priorAction = s_remoteAnimPriorAction[index];
        return true;
    }

    void HeroAnimOnTick(float frame, float priorFrame, unsigned short priorAction)
    {
        s_heroAnimFrame       = frame;
        s_heroAnimPriorFrame  = priorFrame;
        s_heroAnimPriorAction = priorAction;
        s_heroAnimValid       = true;
    }

    bool HeroAnimPrev(float& frame, float& priorFrame, unsigned short& priorAction)
    {
        if (!s_heroAnimValid)
            return false;
        frame       = s_heroAnimFrame;
        priorFrame  = s_heroAnimPriorFrame;
        priorAction = s_heroAnimPriorAction;
        return true;
    }
}
