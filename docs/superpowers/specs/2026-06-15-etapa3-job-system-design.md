# Etapa 3 — Job system fork-join para el build per-entidad (Fase 1 de threading)

> Spec de diseño. Cliente MuMain (worktree `temporal/integration`). Fecha: 2026-06-15.
> Estado: aprobado para escribir plan de implementación.

## 1. Contexto y objetivo

### Objetivo real (aclarado por el usuario 15-jun — corrige el master-plan)
**Bajar el consumo de recursos y sacar el cuello de botella del hilo único.** NO es
perseguir max-FPS (el master-plan `docs/perf/00` apuntaba a >1000 vacío / >500 crowded;
esa tesis queda reencuadrada). No matar la GPU.

### Diagnóstico medido (post-Etapa 1.4)
- El cliente es CPU-bound en **un solo core**: el char/objects pass arma geometría
  per-entidad en el hilo de render. **1 core clavado al 100%**, los otros ~ociosos, GPU
  modesta (swap <1ms; offload de skinning/geo ya hecho en 1.4).
- El cuello NO es el submit GL (barato) ni el sim (~1.3ms): es el **build per-entidad**
  (Animation/bone-build + collect-walk) corriendo serial.

### Qué hace (y qué NO hace) este refactor respecto del consumo
- Threading **redistribuye** el trabajo a N cores; **no reduce el trabajo total**
  (de hecho suma un poco: sync + merge). Saca el cuello (ningún core clavado → fluido,
  sin stutter en multitudes).
- El **consumo total baja solo con un CAP de FPS** (race-to-idle: terminar el frame
  rápido y dormir el resto). **El cap se difiere al final**, después de optimizar todo el
  sistema de dibujado (decisión del usuario). Hasta entonces se mide *build wall-time* y
  *fluidez*, no FPS descapado como "éxito de consumo".

## 2. Decisiones tomadas (brainstorming 15-jun)

| Eje | Decisión |
|---|---|
| Forma de Etapa 3 | **Híbrido por etapas**: job system fork-join ahora; split sim-hilo/render-hilo se evalúa DESPUÉS (el aislamiento de estado es prerequisito de ambos). |
| Aislamiento de estado | **Per-entidad owned** para la data durable de render (bone palette, collect output). Caveat de RAM: el scratch per-vértice (~30MB/set) queda **per-worker thread-local**, no per-entidad (per-entidad serían GBs). |
| Arquitectura | **Fork-join por fase** dentro del frame (G/B/F). Render serial para GL; solo el build paraleliza. |
| Rollout | **Incremental, flag-gated (`MU_JOBS` default OFF), medido**. Paralelismo se enciende ÚLTIMO. |
| Cap de FPS | **Diferido** a después de optimizar el sistema de dibujado. Es el que cobra el ahorro de consumo. |

## 3. Arquitectura — 3 fases por frame

El char pass (y luego objects pass) se parte en:

- **Fase G (gather/cull) — serial, main thread.** Arma la lista plana de entidades
  visibles (`VisibleList`) + snapshot de inputs globales (cámara, alpha de interpolación,
  luces). El cull ya existe; solo se colecta a una lista.
- **Fase B (build) — PARALELA, pool de workers.** `ParallelFor` sobre la lista,
  partición por índice (cada entidad en exactamente 1 worker). Por entidad:
  - Animation advance + bone-matrix build → **`OBJECT->BoneTransform` (owned por la entidad)**.
  - Skin lazy (solo meshes legacy no-instanced) → **arena thread-local del worker**.
  - Collect/InstAdd → **bucket set per-worker** + staging de palette TBO per-worker.
  - **Invariante: cero GL, cero escritura a global mutable compartido.**
- **Fase F (flush) — serial, main thread (dueño de GL).** Merge de los bucket sets
  per-worker → concatenar staging de palette → `TBO.Upload` → `InstFlush` (draws
  instanced). Draws legacy/per-mesh residuales acá (raros post-1.4).

```
[main] cull → VisibleList
[main] FORK
   [workerK] por cada entidad de su tajada:
       Animation+bone-build → entidad.BoneTransform (owned)
       skin lazy (meshes legacy) → arena[K] (TLS)
       collect → buckets[K] + paletteStaging[K]
[main] JOIN
[main] merge buckets[*] → buckets ; concat paletteStaging → TBO.Upload
[main] InstFlush (draws GL) + draws legacy residuales
```

Red ya está off-thread (`IncomingPacketQueue`). Sim sigue serial (corre antes del render;
su estado queda estable/read-only durante Fase B).

## 4. Componentes (nuevos; cada uno un propósito, testeable aislado)

1. **`Core::Jobs::ThreadPool`** — pool persistente mínimo. `ParallelFor(count, fn)`
   bloquea hasta terminar (fork-join). N workers = `min(16, cores−2)` (mismo heurístico de
   concurrencia que el batching de instancing; "16" es tope de hilos, no tiene relación con
   el cap de FPS de 3.6). Sin deps externas. Una instancia para el proceso.
2. **`Render::Build::WorkerArena`** — scratch per-worker: los buffers per-vértice
   (`VertexTransform`/`NormalTransform`/`IntensityTransform`/`LightTransform`/`g_chrome`)
   sacados de file-global a una arena, accedidos vía puntero `thread_local`. ~30MB × N.
   El main thread tiene su arena (flag-off / path serial usa la misma vía → idéntico).
3. **Collect per-worker en `BmdInstanceBatch`** — `s_buckets` global pasa a
   `buckets[workerIndex]` durante Fase B; función de **merge** en Fase F (concatenar los
   arrays de instancias por key). Igual el staging de palette TBO (segmentos per-worker
   concatenados con offsets corregidos).
4. **`Render::Build::VisibleList`** — lista plana per-frame producida por G, consumida por B.
5. **Glue en el char pass** (`RenderCharactersClient` / `SceneManager`) — reestructura el
   loop per-char en fases G/B/F detrás del flag.

## 5. Aislamiento de estado — el refactor load-bearing

- **Scratch per-vértice** (~30MB/set; per-entidad sería 3GB@100ch → imposible):
  file-global → `WorkerArena` vía puntero `thread_local`. El código de Transform/skin
  cambia **solo en el sitio de acceso** (accessor/macro), no su lógica.
- **Bone matrices**: build directo a `OBJECT->BoneTransform` (**ya es `vec34_t*`
  per-objeto**, `w_ObjectInfo.h:216`) en vez del global `BoneTransform[200][3][4]`. El
  global de hierarchy-concat intermedio pasa a miembro de la arena per-worker. Esto es el
  "per-entidad owned": la pose durable vive en la entidad.
- **Buckets/TBO**: per-worker, merge en flush. **Order-independent**: opaco instanced =
  depth-test, additivo = order-independent (GL_ONE/ONE) → el orden de merge no cambia
  píxeles.
- **Read-only compartido en Fase B**: geometría BMD, texturas, cámara, alpha, luces.
  El sim ya corrió → estable en el frame.

## 6. Correctitud y control de riesgo

- **Flag `MU_JOBS` (default OFF)**: OFF = path serial actual **verbatim** (fases colapsan
  al loop de hoy; 0 workers / inline). Cero cambio de comportamiento apagado.
- **Invariante de race** (lo que se audita y revisa): Fase B toca solo (a) data owned por
  entidad (partición por índice, sin compartir), (b) arenas/buckets per-worker,
  (c) read-only compartido. **Ninguna escritura a global mutable.**
- **Audit de globals reentrantes** (paso explícito del plan): globals escritos durante el
  build que NO son per-entidad deben hacerse per-worker / read-only / sacarse afuera.
  Sospechosos conocidos: `g_smodels_total`, caches de chrome por bone
  (`g_chromeage[]`/`g_chromeup[]`/`g_chromeright[]`, `ZzzBMD.cpp:744-766`), cualquier
  scratch estático en el path de Animation. Cada uno se enumera y resuelve.
- **`sizeof(BMD)`** no se toca (array `Models` con offset → crash login; ver
  `crash-login-scene-resuelto`).
- **Validación serial-primero**: el paralelismo se enciende en el ÚLTIMO paso, después de
  que el estado esté aislado y A/B-idéntico en serial. Las races quedan confinadas a un
  solo paso bien entendido.

## 7. Testing / validación

- **Unit**: `ThreadPool` (corre todos los índices, bloquea, maneja count<workers,
  propaga/contiene excepciones); `WorkerArena` (aislamiento por hilo); merge de buckets
  (merged == concat serial, byte-a-byte de instance arrays).
- **Perf A/B** (`run-harness-ab.bat`-style, `MU_JOBS` 0/1, 100ch + 200ch): nueva línea
  `[jobs]` con worker count + ms por fase (G/B/F). Esperado: Fase B wall-time ≈ ÷cores;
  char pass total baja; **build wall-time** es la métrica (no FPS descapado).
- **Visual A/B**: in-game multi-mapa (Lorencia/Devias-nieve/Atlans-agua + especiales),
  **píxel-idéntico** esperado.
- **Race audit** manual riguroso del invariante (+ TSan si es factible en este toolchain).

## 8. Staging (incremental, cada paso flag-gated y medido)

| paso | qué | hilos | A/B esperado |
|---|---|---|---|
| **3.1** | Infra: `ThreadPool` + `WorkerArena` + scaffolding de fases G/B/F | serial | idéntico |
| **3.2** | Mover scratch per-vértice + bone-build a arena/entidad (audit globals) | serial | idéntico (refactor puro) |
| **3.3** | Collect per-worker + merge de buckets/TBO | serial | idéntico |
| **3.4** | **Encender paralelismo** (Fase B vía `ParallelFor`) | **paralelo** | **speedup acá** |
| 3.5 | Extender a objects pass (props) si queda ROI | paralelo | speedup |
| 3.6 | (DIFERIDO, fuera de este spec) **Cap de FPS** configurable → race-to-idle = baja de consumo real | — | consumo ↓ |

## 9. No-goals (de este spec)

- Split sim-hilo / render-hilo (Fase 2 del híbrido; spec aparte, post-aislamiento).
- Render-thread "puro submit" / pipeline cross-frame (bajo ROI: submit ya barato).
- Cap de FPS (diferido a 3.6, después de optimizar el dibujado).
- Perseguir un target de FPS; matar la GPU.

## 10. Riesgos abiertos / a confirmar en el plan

- Cuánto estado global no-per-entidad aparece en el audit (Fase B real) — puede inflar 3.2.
- Costo del merge de buckets (Fase F) vs ganancia del paralelismo a pocos chars (umbral:
  bajo N de entidades, serial puede ganar → el flag/heurístico debe poder caer a serial).
- Reentrancia de llamadas dentro de Animation (alloc/log/estáticos) — parte del audit.
