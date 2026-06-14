#include "stdafx.h"

#include "Core/Diagnostics/RenderHarness.h"

#include "Engine/Object/ZzzCharacter.h"   // CreateHero, CHARACTER, CharactersClient

#include <cstdlib>

// SceneFlag lives in the global scene state (declared the same way the camera units
// reach it). LOG_IN_SCENE etc. come from the scene enum pulled in via the headers.
extern EGameScene SceneFlag;

namespace Core::Diagnostics::RenderHarness
{
    namespace
    {
        // Reserve a high slot range so test chars never collide with the login's own
        // character slots (low indices) or networked players.
        constexpr int kBaseIndex = 200;

        bool s_active = false;

        int EnvInt(const char* name, int def)
        {
            char buf[16] = {};
            size_t n = 0;
            if (getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0)
            {
                const int v = atoi(buf);
                if (v > 0)
                    return v;
            }
            return def;
        }

        int RequestedCount()
        {
            int n = EnvInt("MU_TEST_CHARS", 0);
            const int maxSlots = MAX_CHARACTERS_CLIENT - kBaseIndex;
            if (n > maxSlots) n = maxSlots;
            return n;
        }
    }

    void ApplyTestCharsIfRequested()
    {
        const int n = RequestedCount();
        if (n <= 0 || SceneFlag != LOG_IN_SCENE)
            return;

        static bool s_spawned = false;
        if (!s_spawned)
        {
            s_spawned = true;
            s_active  = true;

            // Grid centred near the world origin (login camera looks toward it). Even
            // off-screen chars still exercise the render path; the screenshot step can
            // re-aim the camera at this grid later.
            const int   cols    = 12;
            const float spacing = 110.f;
            const float cx = -(cols * spacing) * 0.5f;
            const float cy = -(((n + cols - 1) / cols) * spacing) * 0.5f;

            for (int i = 0; i < n; ++i)
            {
                const int idx = kBaseIndex + i;
                const float gx = cx + (i % cols) * spacing;
                const float gy = cy + (i / cols) * spacing;
                // CLASS in the LOG_IN_SCENE branch is ignored (CreateHero forces a Dark
                // Master set + wings/weapons/horse) -> a heavy, mesh-rich char, matching
                // the worst case we measured in-world (~31 mesh draws/char).
                CreateHero(idx, CLASS_KNIGHT, 0, gx, gy, 0.f);
            }
        }

        // Re-assert each frame: the login scene's per-frame culling sets Visible, so we
        // override it just before RenderCharactersClient to guarantee the crowd renders.
        for (int i = 0; i < n; ++i)
        {
            CHARACTER* c = &CharactersClient[kBaseIndex + i];
            c->Object.Live    = true;
            c->Object.Visible = true;
        }
    }

    bool Active() { return s_active; }
}
