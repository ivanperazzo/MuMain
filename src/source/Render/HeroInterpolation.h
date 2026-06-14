#pragma once

namespace Render::HeroInterp
{
    // Stage 2 render interpolation for the Hero. The sim advances the Hero in
    // discrete 25 tps jumps (Stage 1b); drawing at the raw position stutters at
    // high FPS. OnTick() stores the pre-move position each sim tick; the renderer
    // overrides Hero->Object.Position with RenderPos() = lerp(prev, cur, alpha)
    // for the WHOLE draw (camera follow + model both read it), so the scene is
    // smooth at any FPS. Snaps (no lerp) on a teleport-sized jump so the Hero
    // does not visibly slide across the map on warp/spawn. Hero-only for now;
    // remote entities are Stage 3.
    void OnTick(const float curPos[3]);                       // start of each sim tick
    void SetAlpha(float alpha);                               // once per render frame
    void RenderPos(const float curPos[3], float out[3]);      // out = lerp(prev,cur,alpha) or cur on teleport
}
