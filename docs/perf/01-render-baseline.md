# Track GPU / alto-FPS — Deep-dive 01: arquitectura de render actual + plan

> **Objetivo del track (usuario):** "entrar al cliente y tener 1400-2000 fps con una
> NVIDIA 3070 Ti; optimizar el cliente para que renderice por GPU y mejore drásticamente."
> El desacople sim/render (Stages 1b-4b) era el **prerrequisito** y ya está hecho: subir el FPS
> de render ya no acelera el juego (no speedhack). Este track es **separado**.

## TL;DR del hallazgo

El cliente usa **OpenGL legacy fixed-function**, sin VBOs ni shaders, con **skinning de
modelos en la CPU cada frame**. La GPU (3070 Ti) está prácticamente **idle**: el cuello es la
CPU, que transforma y sube geometría vértice a vértice (o array client-side) en cada frame.
Por eso subir el FPS "no usa la GPU" — no hay casi nada residente en GPU.

## Evidencia (conteos en `src/source`)

| Señal | Valor | Lectura |
|---|---|---|
| Contexto GL | `SDL_GL_CONTEXT_PROFILE_COMPATIBILITY`, sin versión pedida, depth 16 | driver da ~GL 2.1 legacy (`SdlShell.cpp:13`, `Winmain.cpp:1397`) |
| `glBegin` | 75 (17 archivos) | immediate-mode presente |
| `glVertex*` | 288 · `glTexCoord` 174 | submission per-vértice |
| `glMatrixMode`/`glTranslate`/… | 33 | matrices fixed-function (no shaders) |
| `glGenBuffers`/`glBindBuffer` | **0** | **sin VBOs** (geometría no reside en GPU) |
| `glUseProgram`/`glCreateShader` | **0** | **sin shaders** (pipeline fija) |
| `glDrawArrays` | 4 · `glVertexPointer` 4 · `glEnableClientState` 8 | algo de vertex-arrays client-side, sin VBO |

Top de `glBegin` (draws calientes): `ZzzLodTerrain.cpp` (18), `ZzzOpenglUtil.cpp` (15, UI/sprites),
`FrustumRenderer` (6), `ZzzBMD.cpp` (4), efectos.

## Cómo se dibuja hoy (por subsistema)

- **Modelos (BMD = personajes/mobs/objetos)** — `Render/Models/ZzzBMD.cpp`:
  - **Skinning en CPU cada frame:** `BMD::Animation` (46) calcula matrices de hueso
    (QuaternionSlerp…) por objeto/frame; `BMD::Transform` (165) recorre **cada vértice** y hace
    `VectorTransform(v->Position, BoneMatrix[v->Node], …)` + normales + lighting
    (`VertexTransform`/`NormalTransform`/`IntensityTransform`/`LightTransform`).
  - Cuerpo principal: `RenderMesh` (944) usa `glVertexPointer`/`glDrawArrays` (array
    **client-side**, re-especificado cada frame, **sin VBO**), **1 draw call por mesh por objeto**.
  - Paths alternativos (chrome/translate/efecto): immediate-mode `glBegin`+`glVertex3fv`
    (`RenderMeshAlternative` 1646, `RenderMeshTranslate` 2134).
  - Costo/frame ≈ Σ_objetos (N_huesos slerp + N_vértices transform CPU + N_vértices submit + draws).
- **Terreno** — `Render/Terrain/ZzzLodTerrain.cpp`: immediate-mode **por tile visible**
  (`glBegin(GL_TRIANGLE_FAN/QUADS)` en `RenderTerrainFace`/`RenderTerrainTile` 1674/1756/1825).
  Grid grande ⇒ muchísimos `glBegin`/vértice/frame.
- **UI / sprites** — `Render/Textures/ZzzOpenglUtil.cpp`: immediate-mode (15 `glBegin`).
- **Present** — `SDL_GL_SwapWindow` (`SdlShell.cpp:83`). VSync toggleable
  (`SDL_GL_SetSwapInterval`, `$vsync off`); al arranque `EnableVSync()` + `SetTargetFps(-1)`.
- **Loop** — render gated por `CheckRenderNextFrame()` (`Winmain.cpp:1106`); la sim ya corre a
  25 tps fijos dentro de `RenderScene` (Stage 1b), desacoplada del render.

## Hipótesis del cuello (a confirmar midiendo)

**CPU-bound**, dominado por (en orden probable):
1. **Skinning CPU + re-spec de arrays client-side** de cada modelo visible, cada frame.
2. **Terreno immediate-mode** por tile (mucho overhead de `glBegin`/vértice).
3. **Draw calls** (1+ por mesh por objeto; sin batching/instancing).
4. UI immediate-mode (menor, salvo HUD pesado).

La GPU casi no trabaja ⇒ el FPS no escala con la 3070 Ti. Subir FPS solo agrega más pasadas de
CPU por segundo.

## Plan por fases (incremental, "favor the simpler path" — CLAUDE.md)

- **P0 — Medir baseline (PRIMERO, gated launch).** Instrumentar tiempo de CPU de render vs
  espera de swap, por frame (extender el CSV: `cpu_render_ms`, `swap_ms`). Capturar a vsync off,
  target ilimitado, en: (a) escena vacía, (b) con muchos mobs/jugadores, (c) ventana chica vs
  grande (test de fill-rate = GPU-bound) y (d) modelos on/off (aislar costo de skinning vs
  terreno). Salida: ¿CPU o GPU bound? ¿qué subsistema domina? Sin esto, optimizar es a ciegas.
- **P1 — Quick wins CPU (bajo riesgo).** Culling antes de skinnear (no transformar modelos
  fuera de frustum/lejanos), evitar trabajo redundante por frame, reducir draw calls obvios.
- **P2 — Geometría estática a VBO.** Terreno + objetos de mapa estáticos subidos 1× a VBO/IBO,
  dibujados con pocas llamadas. Gran baja de overhead per-vértice. No requiere reescribir todo.
- **P3 — Pipeline moderno + GPU skinning (la ganancia grande).** VBO de malla + pesos de hueso,
  matrices de hueso como uniforms, **skinning en vertex shader**. Elimina el skinning CPU y el
  re-upload por frame. Requiere contexto GL 3.3+ (core o compat) + sistema de shaders. Mayor
  esfuerzo/riesgo; hacer después de P0-P2 y solo si la medición lo justifica.
- **P4 — Batching / instancing** de objetos repetidos (mobs iguales, props, partículas).

## Verificación (logs, como en el track temporal)

Cada fase deja CSV analizable: FPS + `cpu_render_ms` + `swap_ms` (+ draw-call count si se puede)
a 30/60/144/ilimitado, en las mismas escenas. Objetivo medible: subir FPS ilimitado y/o bajar
`cpu_render_ms` por fase, sin romper el render (comparación visual A/B opcional). Reusar el
patrón del track temporal: el usuario entra in-game y hace acciones; el log registra.

## Riesgos / notas

- Reescritura del render layer es grande; ir incremental y medible, no big-bang.
- Mantener compat con el editor (`_EDITOR`) y los paths alternativos (chrome/translate).
- GL core profile rompería todo el immediate-mode de golpe ⇒ migrar por subsistema o quedarse en
  compat hasta P3.
- **Primer paso real = P0 (medición), requiere lanzar el cliente (gated por el usuario).**

## P0 — Resultado del baseline (HECHO)

Instrumentación: `Render::FrameProfiler` (std::chrono) parte el frame en `cpu_render_ms`
(armado/submit en CPU: `RenderCurrentScene`+HUD+ImGui) vs `swap_ms` (`PlatformSwapBuffers`).
CSV 16-col + `analyze_perf.py`. Captura `run10_p0.csv` (12822 frames, vsync off, FPS destapado;
secuencia: quieto → zona poblada → ventana chica → ventana grande).

| | fps | frame_ms | cpu_ms | swap_ms | %cpu | %swap | veredicto |
|---|---|---|---|---|---|---|---|
| TOTAL | 68 | 15.5 | 13.1 | **0.44** | **85%** | 3% | **CPU-bound** |

Todos los 12 segmentos: ~85% CPU, ~3% swap (swap_ms ~0.4 ms constante). El resize de ventana
**no cambió** el patrón ⇒ **NO es fill-rate/GPU-bound**. La GPU (3070 Ti) está idle: presenta en
<0.5 ms. El ~12% restante (frame_ms − cpu − swap) es CPU fuera del span medido (sim tick,
`UpdateSceneState`/input, `CalcFPS`, ImGui, loop). ⇒ **frame ≈ 100% CPU**.

⚠️ **Caveat: build DEBUG** (`/Od`, `/RTC1`). Los loops numéricos de skinning (`BMD::Transform`,
slerps) corren sin optimizar ⇒ el FPS absoluto (47-77) está **inflado-bajo**; en Release el
`cpu_render_ms` cae fuerte (típico 3-10× en loops así). La conclusión **CPU-bound** es robusta
(el split 85/3 no depende del build), pero el **techo real de FPS requiere medir en Release**.

**Conclusión P0:** el cuello es 100% CPU; la GPU no se usa. El camino a 1400-2000 fps es mover
trabajo de CPU→GPU (P2 VBO, P3 GPU skinning/shaders) — el diagnóstico valida el plan. Próximo:
medir baseline en **Release** (número real) antes de elegir P1 vs P2 vs P3.

## Estado

P0 hecho (CPU-bound confirmado, GPU idle). **Gate pendiente:** medir baseline Release, luego
elegir fase (P1 quick wins / P2 VBO terreno / P3 GPU skinning). Entrada del track: este archivo.
