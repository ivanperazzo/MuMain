# Etapa — Objects pass: static-prop instancing (KICKOFF, arrancar de 0)

> Doc de arranque para el **último lever view-dependent de FPS** que queda tras cerrar
> terreno (static-VBO, default-ON `fc5d64be`) y chars (instancing + GPU-skin, default-ON).
> Pensado para empezar una sesión nueva sin re-investigar. Leer también
> `MuMain/docs/perf/00-MASTER-PLAN.md` y la memoria `perf-objects-pass-no-hotspot`.

## 1. Objetivo / por qué

El **objects pass** (props del mundo: árboles, casas, antorchas, etc.) es lo que aún hace
swingear los FPS según dónde mire la cámara. Slot `objects` del FrameProfiler:
**~0.5 ms → ~5 ms** según cuántos props entran al frustum (62 → 183 props medidos en
login throne-room). Tras terreno+chars, es el mayor costo CPU restante que escala con la vista.

## 2. Estado MEDIDO (no re-medir desde cero — esto ya se sabe)

Instrumentación `MU_OBJLOG=1` → `gl_obj.txt` (login throne-room, 100 chars):
- Props renderizados: **62 → 183** (view-dependent, count-bound).
- **~77 % son estáticos** (`AnimationFrame == PriorAnimationFrame`, no animan).
- **bone = 0**: NINGÚN prop usa `EnableBoneMatrix` → todos comparten `g_BoneTransformScratch`.
- Split **Calc vs Draw**: **Calc domina** (~1.1–2.2 ms) vs Draw (~0.5–1 ms) a 127 props.
  calc+draw ≈ 83 % del slot.
- El costo de Calc está **REPARTIDO, sin hotspot único**: `BodyLight` (hace
  `RequestTerrainLight` per-prop = sample del terreno) + setup de `CurrentRenderCtx` +
  `Animation` (chico, 1-frame) + bloque Select/edge-scale.

## 3. Approach FALLIDO (no repetir)

**Bone-cache de props estáticos** (`MU_OBJBONECACHE`, revertido): cachear el palette per-`OBJECT*`
y `memcpy` al scratch en vez de recalcular `BMD::Animation` para props estáticos.
A/B same-session: **calc 1.13 → 1.11 ms (CERO ganancia)**. Conclusión: el `Animation()` NO es
el costo del Calc (estos props tienen pocos bones / 1 frame). El lever NO es cachear la animación.

## 4. Infra existente (sobre la que se construye)

**Task 7 — `MU_GPUINSTOBJ`** (`GpuInstObjEnabled`, default-ON) ya instancia props **REPETIDOS**:
props con el mismo BMD se colapsan en un batch instanced (`BmdInstanceBatch`) → 1 draw/textura
por pasada en vez de 1 draw/mesh. Pero la **iteración + Calc per-object sigue corriendo** para
cada prop (la instancing sólo ataca el Draw de repetidos, no el walk ni el Calc).

Entry points de la infra:
- `Render/Models/BmdGpuCache.cpp:267` `GpuInstObjEnabled()` (flag, runtime-toggle `$gpuinstobj`).
- `Render/Models/BmdInstanceBatch.cpp` — el batch GL (merge testeable en `BmdInstanceBatchMerge.cpp`).
- `Render/Models/ZzzBMD.cpp:1763-1771` — donde un mesh de prop decide ir al batch instanced.

## 5. El lever REAL (hipótesis a desarrollar)

A diferencia del terreno (hotspot claro: el walk per-tile), el objects pass es **per-prop
irreducible repartido**. El lever grande, tipo terrain-VBO:

**Bakear/instanciar props ESTÁTICOS enteros** — los ~77 % que no animan. Ideas a evaluar:
1. **Bake estático view-independent** (como el terreno): pre-computar el render de los props
   estáticos del mapa UNA vez (geometría transformada + BodyLight muestreado) a VBOs, y per-frame
   sólo draw — saltando el Calc per-prop (BodyLight/setup) y el walk. Re-bake en cambio de mapa.
   El reto: BodyLight depende de la luz del terreno (¿viva? el terreno ya streamea color per-frame;
   ver si los props necesitan lo mismo o si su luz es estática-suficiente).
2. **Persistent instance buckets**: en vez de re-armar el batch instanced cada frame, mantener
   buckets persistentes de props estáticos por (BMD, textura) y sólo re-cull, evitando el Calc.
3. **Reducir el walk**: `RenderObjects` ya frustum-testea por `OBJECT_BLOCK` (16×16). El costo es
   el per-prop dentro de bloques visibles. Un cull más fino / spatial no ataca el Calc, sólo el count.

Empezar por **medir cuánto del slot es props ESTÁTICOS** (los bakeable) vs animados, para
dimensionar el techo del approach #1 antes de construirlo.

## 6. Entry points del código (file:line)

- **`Engine/Object/ZzzObject.cpp:3290` `RenderObjects()`** — walk 16×16 `OBJECT_BLOCK`,
  `TestFrustrum2D` por bloque, luego per-prop `RenderObject(o, ...)` + `RenderObjectVisual(o)`.
  La llamada general pasa `Translate = GpuInstObjEnabled()` (`:3465`).
- **`Engine/Object/ZzzObject.cpp:2686` `RenderObject()`** — el render per-prop (Calc + Draw).
- **`Engine/Object/ZzzObject.cpp:2773` `RenderObjectVisual()`** — switch por mapa/tipo que crea
  partículas/sprites para props específicos (fuego/humo). Barato para genéricos; los efectos que
  crea son necesarios. NO es el costo principal.
- **`Scenes/SceneManager.cpp:588 / :1183`** — slot `FP::Objects` del FrameProfiler (lo que sale
  como `objects=X.X` en la línea `[frame]` de `gl_log.txt`). `:1183` es el auto-degrade que lo lee.

## 7. Cómo medir (metodología — respetar)

- **Instrumentación**: `MU_OBJLOG=1` → `gl_obj.txt` (props renderizados, % estáticos, split Calc/Draw).
  Si no existe el flag aún, re-agregarlo (estaba en una rama; ver memoria).
- **Slot**: línea `[frame] ... objects=X.X` en `gl_log.txt` (poner `MU_FPS`/correr el harness).
- **Harness**: `MU_TEST_CHARS=N` (crowd), `MU_TEST_SHOT=<frame>` (screenshot `harness_shot.jpg`).
  Ver `Core/Diagnostics/RenderHarness.cpp`.
- **A/B**: SIEMPRE same-session back-to-back + ventana fija larga en steady-state (NO frames de
  carga). Piso de ruido ~1 ms / 6-8 % run-to-run — una "ganancia" menor a eso es ruido
  (lección de `perf-noise-floor-animlod-jobs`). Cada flag con override `=0` para A/B.
- **Build**: `I:\MuOnline\build_main_release.bat` (main, ninja incremental). Correr/medir:
  server `launch_server.bat` + cliente `launch_client_perf.bat` (ver `I:\MuOnline\LOCAL_TESTING.md`).
  **`MU_JOBS` queda en 0** (deadlock, ver `threadpool-parallelfor-deadlock`).

## 8. Primeros pasos sugeridos (de 0)

1. Re-habilitar `MU_OBJLOG`, correr el harness crowd, confirmar el split actual (props estáticos
   vs animados, Calc vs Draw) en el build current default-ON.
2. Medir el **techo**: ¿cuánto del slot `objects` desaparecería si los props estáticos costaran 0?
   (p.ej. flag temporal que skipee el render de props estáticos y medir el delta — feo pero acota).
3. Si el techo vale la pena (>1-2 ms reales), diseñar el **static-prop bake** (approach #5.1):
   prototipo gated `MU_OBJSTATIC`, default-OFF, validar A/B + visual (igual que se hizo con el terreno).
4. Cuidado con la **luz**: validar si los props estáticos necesitan BodyLight vivo (fuego/antorchas
   pulsan) — si sí, hay que streamear su color como hace el terreno, no congelarlo.

## 9. Riesgos / incógnitas

- Esfuerzo **grande** (tipo terrain-VBO, ~medio día+), no win rápido. Confirmar el techo primero.
- **Luz viva** de props cerca de fuego/efectos (no congelar y que se note, como pasó con el pasto).
- Props que parecen estáticos pero tienen efectos asociados (`RenderObjectVisual`) — no romperlos.
- Alternativa barata pero sucia: bajar `RENDER_OBJECT_DIST` → pop-in (no limpio, último recurso).

---
**Estado al abrir**: nada empezado. Terreno + chars ya cerrados (default-ON). Este es el lever
restante. Relacionado: [[perf-objects-pass-no-hotspot]], [[perf-terrain-walk-bottleneck]],
[[perf-noise-floor-animlod-jobs]], [[gpu-highfps-track]].
