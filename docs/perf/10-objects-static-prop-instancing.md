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

### 2b. Re-medido 18-jun (build main default-ON, `MU_OBJLOG` re-añadido) — TECHO confirmado
`MU_OBJLOG` re-instrumentado en `RenderObject()` (timing Calc/Draw + split static/animado),
emitido cada 30 frames desde `RenderObjects()`. Harness `measure_obj_main.bat` (main Release,
100 chars). Resultados:
- Props **54 → 252** (count-bound, confirma view-dependence).
- **~89–93 % estáticos por count** en este ángulo (más que el 77 % previo; depende de la vista).
- **Calc sigue dominando** Draw ~2:1; calc+draw instrumentado ≈ el slot real (overhead chrono NO domina).
- Slot `objects` real (línea `[frame]`): **pico ~5–6 ms** en ventana cargada (la "1.5 ms" es la cola
  con cámara asentada — no usar).
- **Static props = ~63 % del TIEMPO del pass** (no sólo del count) → **TECHO del bake ≈ 2–2.7 ms
  steady high-count, ~3–3.8 ms en el pico**. Supera el umbral 1–2 ms del §8.3 → **el static-prop
  bake (approach §5.1) VALE la pena construirlo.**
- Contexto: `chars` 8–17 ms sigue siendo el slot #1 del frame; `objects` es el #2 y el único
  lever view-dependent que queda (terreno+chars ya cerrados default-ON).

### 2c. EL COSTO REAL — `BMD::Transform`/`SkinMesh`, NO BodyLight (18-jun) — la hipótesis del §2/§5 era ERRÓNEA
Sub-breakdown del Calc de static props (`MU_OBJLOG`, timing por sub-parte):
- **bodylight = 0.02–0.03 ms** (¡NEGLIGIBLE! `RequestTerrainLight` es trivial post terrain-VBO).
- anim = 0.12–0.16 ms (chico, confirma el bone-cache fallido).
- **xform = 1.2–1.4 ms = ~85 % del static Calc** ← EL COSTO.
`BMD::Transform` (ZzzBMD.cpp:379) corre un loop `SkinMesh()` que **CPU-skinea todos los vértices
de cada mesh, cada frame**. Para props NO aplica el `deferActive` del char-pass (pide `GpuCharsPass()`),
así que props se dibujan GPU-instanced pero IGUAL se CPU-skinean en Transform — **redundante**: el
draw instanced skinea en GPU (bone-palette TBO), nadie lee `VertexTransform` salvo consumers CPU raros
(mesh no-instanced, shadow, efectos), que ya force-skinean lazy vía `EnsureMeshSkinned` (ZzzBMD.cpp:1923).
En runtime (`EditFlag != 2`) `SkinMesh` ni siquiera computa bbox → el OBB usa el bbox precomputado del
prop → el defer NO afecta culling/picking.

**El lever NO es el light-VBO bake del §5.1 — es el skin-defer de props.**

### 2d. PROTOTIPO `MU_OBJSKIN` (default-OFF) — VALIDADO (18-jun)
Extendido el `deferActive` de `BMD::Transform` al objects pass (gate: objects-pass + GPU
bmd/instobj/shadow), flag `GpuObjSkinDeferEnabled()` (`MU_OBJSKIN=1`, default-OFF). A/B same-session
(login throne-room, 100 chars):
| | OFF | ON |
|---|---|---|
| objects slot pico | ~4.8–5.7 ms | **~3.1–3.4 ms** |
| static calc | ~1.5 ms | ~0.3 ms |
| static xform | ~1.3 ms | **~0.08 ms** |

**~1.7–1.9 ms off el objects pass (~35 %)**, justo el techo medido. **Visual A/B idéntico**
(`shot_objskin_off/on.jpg`): estatuas, antorchas+fuego, gárgolas, puertas — todo correcto, sin
geometría basura ni props faltantes. Bajo riesgo (reusa infra char-defer + lazy `EnsureMeshSkinned`).
**PENDIENTE para default-ON: validación IN-GAME** (login throne-room ≠ MainScene; props de town
—árboles/casas/picking— pueden tener consumers CPU no ejercitados acá). Commit prototipo default-OFF.

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

1. ✅ HECHO (18-jun) — `MU_OBJLOG` re-añadido en `RenderObject()`/`RenderObjects()`
   (gated, default-OFF, zero-cost cuando unset). Harness `measure_obj_main.bat`. Split confirmado
   (ver §2b): count-bound 54→252, ~89-93 % static por count, Calc domina Draw.
2. ✅ HECHO (18-jun) — Techo medido vía split static/animado del TIEMPO (no skip flag, no cambia
   visual): **static props = ~63 % del tiempo del pass; ceiling real ≈ 2-2.7 ms steady, ~3-3.8 ms
   pico**. Slot real pico ~5-6 ms. **Supera 1-2 ms → seguir al paso 3.**
3. ⏭️ PRÓXIMO — techo vale la pena (>1-2 ms reales) → diseñar el **static-prop bake** (approach #5.1):
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
