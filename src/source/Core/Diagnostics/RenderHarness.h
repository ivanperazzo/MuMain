#pragma once

namespace Core::Diagnostics::RenderHarness
{
    // Autonomous render/perf test harness. Spawns N fully-equipped test players in
    // the login scene (MU_TEST_CHARS=N) so the Characters render pass, instancing
    // and the coverage log run with a real crowd WITHOUT a server. Env-gated and a
    // no-op when MU_TEST_CHARS is unset, so normal play is untouched.
    //
    // Call ApplyTestCharsIfRequested() each frame right before RenderCharactersClient
    // in the login scene: it spawns once, then re-asserts Live/Visible every frame so
    // frustum culling can't drop the crowd before it reaches the render path.
    void ApplyTestCharsIfRequested();

    // True when MU_TEST_CHARS spawned a crowd (so the scene can fix the camera, etc.).
    bool Active();

    // Screenshot for autonomous A/B visual checks (MU_TEST_SHOT=frameN): once frame N
    // is reached, glReadPixels the back buffer and write "harness_shot.jpg" next to the
    // exe. Call after the frame's draw submission, before the buffer swap. No-op when
    // MU_TEST_SHOT is unset. The runner tags the file (copy to shot_<config>.jpg).
    void CaptureShotIfRequested();
}
