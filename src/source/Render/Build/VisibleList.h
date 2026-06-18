#pragma once

// Per-frame flat list of characters that will render this frame, plus the
// per-entity render inputs computed during Phase G (cull + interpolation).
// Phase B consumes one entry per worker: it reads only the entry (no shared
// decision state), applies the precomputed pose/position onto the entity,
// renders, and restores. This is the load-bearing isolation for the jobs work
// (Etapa 3, Task 5) — see docs/perf/08-jobs-audit.md.

struct CHARACTER;
struct OBJECT;

namespace Render::Build
{
    struct VisibleChar
    {
        CHARACTER*     c;
        OBJECT*        o;
        int            index;            // CharactersClient[] slot

        // Interpolated render position (Phase G). For remote entities this is
        // the smoothed position; for the Hero it equals the raw sim position
        // (the Hero is interpolated for the whole draw in RenderScene).
        float          renderPos[3];
        bool           applyRenderPos;   // true => override o->Position in Phase B

        // Interpolated animation pose (Phase G). Applied to the entity for the
        // whole RenderCharacter call, then restored.
        float          animFrame;
        float          animPriorFrame;
        unsigned short animPriorAction;
        bool           applyAnim;        // true => override anim fields in Phase B

        bool           selected;         // i == SelectedCharacter || i == SelectedNpc

        // Distance anim-LOD (Phase G decision): when true, Phase B reuses the cached
        // bone pose instead of rebuilding it (far char, off-schedule frame).
        bool           skipBones;
    };
}
