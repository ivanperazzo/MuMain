#include "stdafx.h"

#include "Core/Diagnostics/RenderHarness.h"

#include "Engine/Object/ZzzCharacter.h"   // CreateHero, CHARACTER, CharactersClient
#include "Render/Textures/ZzzTexture.h"   // WriteJpeg
#include "Render/GL/GLLog.h"
#include "Render/Terrain/ZzzLodTerrain.h" // RequestTerrainHeight (stand grid on ground)
#include "Camera/CameraState.h"           // g_Camera (frame the test grid)

#include <gl/glew.h>
#include <cstdlib>
#include <vector>

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
            for (int i = 0; i < n; ++i)
            {
                // CLASS in the LOG_IN_SCENE branch is ignored (CreateHero forces a Dark
                // Master set + wings/weapons/horse) -> a heavy, mesh-rich char, matching
                // the worst case we measured in-world (~31 mesh draws/char).
                CreateHero(kBaseIndex + i, CLASS_KNIGHT, 0, 0.f, 0.f, 0.f);
            }
            Render::GL::Log("[harness] cam pos=(%.0f,%.0f,%.0f) ang=(%.0f,%.0f,%.0f) dist=%.0f",
                g_Camera.Position[0], g_Camera.Position[1], g_Camera.Position[2],
                g_Camera.Angle[0], g_Camera.Angle[1], g_Camera.Angle[2], g_Camera.Distance);
        }

        // Re-place + re-assert each frame: centre the grid on the camera's look-at so the
        // crowd is framed, and override Visible (the scene's culling runs before us).
        const int   cols    = 12;
        const float spacing = 90.f;
        const float cx = g_Camera.Position[0] - (cols * spacing) * 0.5f;
        const float cy = g_Camera.Position[1] - (((n + cols - 1) / cols) * spacing) * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            CHARACTER* c = &CharactersClient[kBaseIndex + i];
            const float px = cx + (i % cols) * spacing;
            const float py = cy + (i / cols) * spacing;
            c->Object.Position[0] = px;
            c->Object.Position[1] = py;
            // Stand the grid on the ground (the login camera looks DOWN at the terrain;
            // placing chars at the camera eye height left them above the visible frame,
            // so the harness screenshot never showed the crowd or its shadows).
            c->Object.Position[2] = RequestTerrainHeight(px, py);
            c->Object.Live    = true;
            c->Object.Visible = true;
        }
    }

    bool Active() { return s_active; }

    void CaptureShotIfRequested()
    {
        const int target = EnvInt("MU_TEST_SHOT", 0);
        if (target <= 0)
            return;

        static int  frame = 0;
        static bool done  = false;
        ++frame;
        if (done || frame < target)
            return;
        done = true;

        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);
        const int w = vp[2], h = vp[3];
        if (w <= 0 || h <= 0)
            return;

        std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf.data());

        // WriteJpeg uses TJFLAG_BOTTOMUP, matching glReadPixels' bottom-left origin -> no flip.
        wchar_t fn[] = L"harness_shot.jpg";
        const bool ok = WriteJpeg(fn, w, h, buf.data(), 92);
        Render::GL::Log("[harness] screenshot %ls %dx%d frame=%d -> %s",
            fn, w, h, frame, ok ? "OK" : "FAIL");
    }
}
