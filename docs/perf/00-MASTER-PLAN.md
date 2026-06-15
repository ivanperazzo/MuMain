# Plan maestro — Optimización del motor gráfico (MuMain)

> Roadmap único de TODAS las mejoras del render, en etapas ejecutables. Ata la
> serie `docs/perf/01..06` (deep-dives por tema) y agrega las etapas futuras.
> Cada item: estado, ROI esperado, riesgo, dependencia. Ejecutar por etapas,
> A/B in-game cada cambio, detrás de flag, default-on al validar.

## 0. Contexto y tesis

El cliente es el motor MU legacy: **immediate-mode fixed-function GL, un solo hilo**.
Medido CPU-bound: **GPU 3080/3070Ti ociosa** (swap <1ms), 100% del frame es CPU
armando/enviando geometría vértice-a-vértice. Targets usuario: **>1000fps vacío,
>500 con 100+ chars + efectos**. Es un juego 2005 (tex 256², mapas chicos) → el
techo lo pone el desperdicio CPU, no la GPU.

**Tesis de optimización (3 ejes):**
1. **Sacar trabajo del CPU al GPU** — batch/instancing/VBO/shaders (la GPU está libre).
2. **No recalcular lo que no cambió** — cachear (texto, bone matrices, geometría estática).
3. **Sacar trabajo del hilo de render** — threading (sim/red/IO/anim en paralelo).

Métrica: `[frame]` log (FrameProfiler: terrain/objects/chars/items/effects/sim/cloth/
flush/anim/sprites/uileg/uinew) + `MU_TEST_CHARS` harness + A/B in-game.

## 1. LÍNEA BASE — ya hecho (no rehacer)

| Área | Qué | Doc / commit |
|---|---|---|
| Timing | Decouple sim/render: fixed 25tps + interpolación (Stages 1b-4b) | render-decouple |
| BMD GPU | GPU skinning por-mesh + cache de geometría | `03/04-bmd-gpu` |
| BMD instancing | chars idénticos → 1 draw; **chrome variants** (CHROME2/3/4/6/METAL) | `05-bmd-instancing`, ca84ae96 |
| Skip-skin | defer skinning CPU en instanced; lazy `EnsureMeshSkinned` | 009b1ca1 |
| Sombras | instanced GPU shadows (stencil) | 8b52f2ff |
| Loops desperdicio | `g_chrome` + `LightTransform` reubicados tras early-return (skip en instanced) | ff2cd8e4, d4d760d3 |
| Terreno | **VBO batched fixed-function** (default ON), terrain 2.7-6.4→0.3-0.5ms | `02-terrain-vbo`, ccf18363 |
| Vsync | `MU_NOVSYNC` (uncapped, para medir/llegar al target) | 7137ac69 |
| Profiling | FrameProfiler completo + per-window UI tracer | 2a809846 |

**Resultado actual:** render harness 60→11ms (3.7x), crowded ~5→~50fps, empty ~200→
~300fps, terrain 6-10x. **Breakdown crowded (~20ms cpuRender, GPU ociosa):** chars ~7
+ objects ~4 + **uinew ~5.5** + terrain ~0.4 + sim ~1.3 + anim ~1.3(subset).

**Techo:** >500 crowded NO alcanzable single-thread → requiere Etapa 3 (threading).
sim/cloth descartados como cuello (medidos chicos).

## 2. ETAPA 1 — Wins CPU dirigidos (single-thread, riesgo bajo/medio) ← EMPEZAR ACÁ

Incrementales, alto ROI inmediato. Orden por ROI:

- **1.1 — UI text cache (H1). HECHO ✅** Medido in-game `uinew 5.5→1.38ms` (~4x),
  hit 98–99 %, sin regresión visual; **default ON** (`MU_UITEXTCACHE=0` desactiva).
  Cache string→textura GL propia (key texto+color+HFONT), HIT salta GDI+upload.
  Archivos `UI/Legacy/UITextCache.{h,cpp}` + `UIControls`. → `06-ui-text-cache`.
- **1.2 — UI hidden-update skip + manager cleanup (H2/H3).** Ventanas ocultas dejan
  de updatear; CNewUIManager sin 4 sorts + 3 copias/frame. ~0.5-1ms. → `06`.
- **1.3 — Char RenderMesh-walk: saltar estado GL redundante en collect.** ~2200-3100
  meshes hacen BindTexture+blend aunque solo se colecten (el flush re-setea). El walk
  es ~6.8ms de los 7 de chars. Skip GL-state cuando se instancia → chars ~7→~4-5ms.
  Riesgo medio (hot-path `ZzzBMD::RenderMesh`), A/B harness.
- **1.4 — Objects pass (~4ms).** Props del mapa usan el MISMO `BMD::Transform`/
  `RenderMesh`. Aplicar instancing (props repetidos) + rigid-VBO (props estáticos, sin
  transform CPU/frame). → reusa infra de `03/05`.
- **1.5 — Flip defaults GPU ON** (BMD/inst/shadow/skin) tras validar multi-mapa.
  Hoy son opt-in (run-temporal-rel.bat). Que el cliente normal los use.

**ROI Etapa 1:** crowded ~50→~80-100fps, empty ~300→~500fps (cerca del target vacío).

## 3. ETAPA 2 — Pipeline GL moderno (medio; habilita offload profundo)

Migrar de fixed-function a VBO/VAO+shaders donde aún no (la infra de shaders ya
existe parcial: instanced BMD shaders).

- **2.1 — Terreno Enfoque B** (el salto del `02`): texture-array/atlas + shader que
  hace blend Layer1/Layer2 + lighting (lightmap atributo + Luminosity uniform + luces
  dinámicas), geometría estática en VBO + color streameado. Pocos draws, terreno
  0.4→~0.1ms, libera las 2 pasadas. (El batched actual ya da 6-10x; esto es el extra.)
- **2.2 — Props estáticos → rigid VBO** (geometría una vez, sin transform CPU/frame).
- **2.3 — Texto → glyph atlas batcheado** (versión completa de 1.1: un draw por
  string/atlas, no quad-por-glifo).
- **2.4 — Efectos/sprites/partículas → batched** (RenderSprites/Particles inmediato).

## 4. ETAPA 3 — Threading (Fase B) — el techo real (riesgo alto, grande)

Único camino a los kfps del target. Su propio ciclo brainstorm→spec→plan.

- **3.1 — Sim en su hilo** (ya es fixed-step determinista → moverlo fuera del render).
- **3.2 — Network/IO fuera del hilo render** (parcial: `IncomingPacketQueue`).
- **3.3 — Render thread = puro submit** desde un snapshot/command-buffer (doble buffer
  de estado sim→render, ya hay interpolación que ayuda).
- **3.4 — Job system para anim/bone-build per-char** (paralelizar el Transform/Animation).

**ROI:** rompe el techo single-thread → hacia >500 crowded / kfps vacío.

## 5. ETAPA 4 — Rediseño UI retained-mode (grande; paralelizable con Etapa 3)

Si se quiere el salto estructural en vez de los parches 1.1/1.2. Render-to-texture +
redibujar solo on-change; un solo sistema de ventanas (hoy coexisten 3). Ver auditoría
`docs/ui/UI_MAP.md` (E1-E12) y memoria `ui-window-system-redesign-future`.

## 6. ETAPA 5 — Modernización del motor (futuro, CLAUDE.md "reemplazar engine")

Opcional/último: reemplazar fixed-function por un forward renderer moderno (assets ya
existen, favorecer el camino simple). Solo si las Etapas 1-4 no bastan.

## Secuenciación recomendada

```
Etapa 1 (ahora, independiente, bajo riesgo, máximo ROI inmediato)
   1.1 → 1.2 → 1.3 → 1.4 → 1.5
Etapa 2 (cuando se quiera más offload GPU; necesita infra shader, parcial)
Etapa 3 (threading) ── único camino a >500 crowded; DESPUÉS de exprimir single-thread
Etapa 4 (UI redesign) ── reemplaza 1.1/1.2 si se va a lo grande; si no, los parches bastan
Etapa 5 (rewrite engine) ── solo si hace falta
```

## Reglas transversales (todas las etapas)

- Cada cambio detrás de env flag (`MU_*`), default OFF hasta validar, flip ON al probar.
- A/B in-game multi-mapa (Lorencia/Devias-nieve/Atlans-agua/mapas especiales:
  Kanturu/CursedTemple/Empire/Karutan/BattleCastle) — los casos especiales rompen.
- Medir antes/después con `[frame]` + harness. No “se siente más rápido”: número.
- No tocar `sizeof(BMD)` (array `Models` con offset; crash login). Contexto Transform
  en statics, no miembros. Ver `crash-login-scene-resuelto`.
- Lanzar SOLO via launch_client.bat / run-temporal-rel.bat (cmd.exe); bash mangea /u/p.

## Decisión estratégica abierta

El objetivo REAL del proyecto (CLAUDE.md) es **server-authority**, no gráficos
(“optimize/replace graphics engine = future goal, not yet started”). Ya logramos 10x
en FPS. Opciones: (a) seguir Etapa 1 (wins baratos, bajo riesgo) y parar antes de
threading; (b) ir full a threading por el target; (c) cerrar perf y volver a
server-authority. Recomendación: **Etapa 1 completa** (mejor relación esfuerzo/ganancia),
luego reevaluar threading vs server-authority.
