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
}
