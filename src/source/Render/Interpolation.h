#pragma once

namespace Render::Interpolation
{
    // Component-wise linear interpolation from a to b by alpha (clamped to
    // [0,1]). Pure, no engine deps -> unit-testable. out may alias a or b.
    void Lerp(const float a[3], const float b[3], float alpha, float out[3]);

    // Lerp(prev, cur, alpha) but SNAPS to cur when prev->cur is a teleport-sized
    // jump (warp/spawn/server correction), so an entity never slides across the
    // map. Used by every entity renderer.
    void LerpGuarded(const float prev[3], const float cur[3], float alpha, float out[3]);

    // Master on/off for render interpolation (console: $interp on|off). When off,
    // LerpGuarded returns cur, so entities render at the raw 25 Hz sim position —
    // for A/B comparison of the smoothing. Default on.
    void SetEnabled(bool on);
    bool Enabled();

    // Separate on/off for Stage 4b BODY POSE interpolation (console:
    // $poseinterp on|off). Default OFF while it is being validated, so the
    // snapshot + per-draw frame override are completely inert unless turned on.
    void SetPoseEnabled(bool on);
    bool PoseEnabled();

    // Shared render interpolation alpha for the current frame (set once per frame
    // from SimulationClock; read by all entity renderers).
    void  SetFrameAlpha(float alpha);
    float FrameAlpha();

    // Real wall-clock duration of the current render frame, in ms (set once per
    // frame from the fixed-step driver). Used by render-path animation advance to
    // scale by real time instead of the sim's pinned FPS_ANIMATION_FACTOR, so
    // attached-part animations don't speed up at high FPS. 0 outside MAIN_SCENE.
    void   SetFrameMs(double ms);
    double FrameMs();

    // Per-slot (CharactersClient[index]) previous-tick position, for interpolating
    // remote entities (mobs / other players) between fixed sim ticks.
    // RemoteOnTick snapshots the pre-move position each tick; RemoteRenderPos
    // returns LerpGuarded(prev, cur, FrameAlpha).
    void RemoteOnTick(int index, const float curPos[3]);
    void RemoteRenderPos(int index, const float curPos[3], float out[3]);

    // Stage 4b: per-slot previous-tick BODY animation state (frame + prior
    // keyframe), for interpolating the pose between ticks. Deliberately kept OUT
    // of the OBJECT struct (parallel arrays, like the position store) so the widely
    // included object header is untouched. RemoteAnimPrev returns false until the
    // first snapshot for that slot.
    void RemoteAnimOnTick(int index, float frame, float priorFrame, unsigned short priorAction);
    bool RemoteAnimPrev(int index, float& frame, float& priorFrame, unsigned short& priorAction);

    // Hero body animation prev, snapshotted in MainSceneFixedUpdate (the Hero is
    // tracked separately from remotes, like HeroInterp for position).
    void HeroAnimOnTick(float frame, float priorFrame, unsigned short priorAction);
    bool HeroAnimPrev(float& frame, float& priorFrame, unsigned short& priorAction);
}
