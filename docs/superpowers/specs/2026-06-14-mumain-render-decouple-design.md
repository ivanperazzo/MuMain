# MuMain — Desacople de render y costo por frame para alto FPS (diseño)

Fecha: 2026-06-14 · Worktree `I:\MuOnline\MuMain-temporal` (branch `temporal/integration`)

## Problema

El cliente (Main 5.2, OpenGL modo inmediato, 2005) no escala FPS aunque la GPU (RTX 3080/3070Ti)
esté ociosa. Dos causas, ortogonales:

1. **Costo de render por frame** (techo de FPS). Medido: pasada Characters = ~2640 draws/frame
   (88 chars × ~30 meshes), modo inmediato + **skinning CPU per-vértice** (`BMD::Transform`).
   Crowd → ~6fps (DEBUG). La GPU presenta en <0.5ms (idle). 100% CPU-bound en submission.
2. **Acoplamiento single-thread** (estabilidad). Todo (sim, red, IO/assets, render, present) corre
   serial en un hilo dentro de `RenderScene()`. Cualquier hitch (carga de asset, GC de red, etc.)
   frena el frame. El timing sim/render YA se desacopló (fixed 25tps + interpolación, Stages
   1b-4b), pero la EJECUCIÓN sigue en un hilo.

**Decisión (usuario): atacar ambos, EN ORDEN.** Primero costo de render (sube el techo), luego
threading (estabilidad). Razón: en escena vacía ya hay ~573fps (DEBUG); el techo de crowd lo fija
el costo de render, no el threading. Threading no acelera draws (GL es single-thread por
contexto), solo evita que stalls externos tiren el frame.

## No-objetivos (YAGNI)

- No reescribir el engine ni portar a OGL 3 core / sacar el modelview (el foro lo sugiere; es otro
  proyecto). Se mantiene el contexto compat 4.6 y se mezclan draws modernos con legacy.
- No tocar lógica de juego. No multithreading de GL (imposible en modo inmediato compat).
- No sombras instanciadas al inicio (pasada aparte; evaluar después).

## Métrica de éxito

- Crowd (cuando el usuario pueda reproducir): `draws/frame` de ~2640 → cientos; `chars_ms` ↓ fuerte.
- Solo/autónomo: compila; entra a MAIN_SCENE sin crash (cdb); shader instanciado compila
  (`gl_log.txt`); el path instanciado ejecuta (contador); A/B idéntico (validación visual del
  usuario luego). Medir también en **Release** (DEBUG infla 5-10×).

---

## FASE A — Costo de render (instancing + GPU skinning)

Ya hecho (P-bmd-gpu, commits hasta 64f76276): GPU skinning per-mesh para props (Objects) y
personajes (Characters) detrás de `$gpubmd`, modo lit (props) + flat (chars). **Ganó en props
(17→6ms), NO en crowd de chars** porque no reduce el número de draws y `Transform` sigue
skinneando en CPU. Fase A real = **instancing** (colapsar draws) + **skip de skinning CPU**.

### Enabler verificado
Chars usan `Translate=true` ⇒ posición mundo en `BodyOrigin` (horneado), rotación en bone
matrices, escala en `BodyScale`; **modelview = vista de cámara COMPARTIDA** (el único
`glTranslate` per-char es `RenderGuild`, no el cuerpo). ⇒ un `glDrawArraysInstanced` puede
dibujar N chars con `uViewProj` compartido + datos por instancia + bone palettes en TBO.

### Arquitectura (detalle en `docs/perf/05-bmd-instancing.md`)
Dos fases por frame en la pasada Characters:
- **COLLECT**: por char, copiar su bone palette a un buffer y guardar `paletteBase`; por mesh
  elegible, push de `InstanceRec {paletteBase, bodyScale, bodyOrigin, color, lit}` a un bucket
  keyed por `(BMD*, meshIndex, texId, flagClass)`.
- **FLUSH**: subir palettes a un TBO (`samplerBuffer`); por bucket, subir InstanceRecs como
  atributos instanciados (divisor 1) + `glDrawArraysInstanced`. VS skinnea desde el TBO.

### Ciclos de Fase A (incrementales, build + cdb-smoke + commit c/u)
- **A0 — Auto-enter dev** (habilita test autónomo): flag/env para saltear login+char-select y
  entrar a MAIN_SCENE con la cuenta de test, así cdb autónomo llega a renderizar personajes.
- **A1 — GLLoader += instancing**: `DrawArraysInstanced`, `VertexAttribDivisor`, `TexBuffer`
  (+ verificar GL≥3.1). Smoke: `IsLoaded` sigue ok.
- **A2 — BonePaletteTBO** (`Render/GL/`): buffer RGBA32F + textura buffer; `Begin/AppendPalette
  ->base/Upload/Bind`. Self-test mínimo.
- **A3 — InstancedBmdShader**: VS skin desde TBO + per-instance attribs + `uViewProj`; FS
  textura×color; lit/flat. Compila al 1er uso (log a archivo).
- **A4 — BmdInstanceBatch** (`Render/Models/`): `Begin/Add/Flush`; buckets; reusa `BmdGpuCache`
  para geometría base. `$gpuinst on/off`.
- **A5 — Integración**: en pasada Characters, si `$gpuinst`, COLLECT (no draw) + FLUSH al final.
  Elegibilidad = la de P-bmd-gpu + `Translate==true`. No-elegibles → legacy.
- **A6 — Skip skinning CPU**: para meshes que van instanciadas/GPU, evitar el per-vértice de
  `Transform` (lazy per-mesh; cuidado con lectores de `VertexTransform`: física cloth, sombras,
  SideHair, maps). Saves ~mitad del costo (medido con `$skinskip`).
- **A7 — Medir + ampliar**: draws off/on, chars_ms; luego más flagClasses (chrome/blend) y props.

### Riesgos Fase A
Modelview no-compartido en algún caso → gate por `Translate==true`. Límite uniforms/TBO →
cap de huesos (S6 tiene pocos). Estado GL legacy/moderno contaminado → restaurar siempre.
A/B visual debe ser idéntico (misma matemática ya validada en P-bmd-gpu). Rollback: `$gpuinst off`.

---

## FASE B — Threading / desacople de ejecución (estabilidad)

Objetivo: que stalls de sim/red/IO NO tiren el frame; el render corre a su ritmo. GL es
single-context por naturaleza ⇒ el hilo de render SIEMPRE serializa la submission. Threading
NO sube el techo de FPS (eso es Fase A); **aísla hitches** del present.

### Encuadre autoritativo (clave para T3)
El server (OpenMU) es autoritativo: movement (cadence cap + divergence brake, `75df78367`),
skills (attack-speed cadence + explicit-hit + auto-cast loop, `3818ef296`/`9b936aaf1`/`24c4f59a7`),
melee engagement (`9a6d38f65`). El cliente NO simula verdad: **predice + interpola hacia el estado
del server**. Por lo tanto el "hilo de sim" del cliente es realmente predicción/interp +
procesamiento de paquetes; el snapshot que publica al render DEBE reconciliarse con el estado
autoritativo entrante (no pisarlo). El bug abierto monster-position-desync (`ReceiveMoveCharacter`
ignora `SourceX/Y` autoritativo) es exactamente un defecto de esa reconciliación y se cierra
dentro de T3.

> **Dependencia (diferida por decisión del usuario, 2026-06-14):** `temporal/integration` está
> atrás de `main` en 5 commits autoritativos del cliente (`4887882d` clientlib←OpenMU local,
> `66bb9f26` AttackIntent/StopAttackIntent, `4406d803` packet XML local, `25d8f4ae` engagement
> fix, `83338c65` shutdown rc=139). El merge es limpio (solo `FrameTimerScheduler.cpp` +
> `cdb-crash.txt` se solapan). **Reconciliar (merge main → temporal) ANTES de ejecutar T2/T3**,
> porque el packet-processing y el snapshot se construyen sobre esa base. Hoy: NO integrado.

### Modelo objetivo
- **Hilo de render = main**: dueño del contexto GL + eventos de ventana SDL. Lee el último
  snapshot publicado, interpola (Stages 3/4 ya leen prev/cur ⇒ encaja) y hace todo el dibujo +
  present, uncapped. **GL y ventana NUNCA se mueven de main** ⇒ sin handoff de contexto. Diverge
  del boceto previo (B2 movía GL a otro hilo) a propósito: mover GL agrega riesgo sin beneficio.
- **Hilo de sim/lógica = worker**: SimulationClock fixed 25tps + AI + movimiento + procesamiento
  de paquetes; publica snapshots. Compute puro, sin GL ni ventana.
- **I/O/assets = worker(s)**: carga de texturas/modelos fuera de main; el upload GL queda en main.

### Ciclos de Fase B (de menor a mayor riesgo; build + cdb-smoke + commit c/u)
- **T0 — Instrumentación per-subsistema.** Extender `FrameProfiler`/`[frame]` con `ioWait,
  packet, sim, skin, submit, present` + detector de hitch (frame > 2× mediana lo loguea). Sin
  esto no se prioriza ni se valida cada ciclo. Bajo riesgo.
- **T1 — I/O de assets async.** Worker lee archivo + decodifica a bitmap CPU; el upload GL queda
  en main (cola acotada N/frame, evita picos). Productor/consumidor. Mayor fuente de hitch,
  frontera limpia, bajo riesgo de estado.
- **T2 — Packet processing fuera del frame.** Hoy `DrainTo` procesa TODOS los encolados por-frame
  ⇒ ráfaga = hitch. Primer paso: cap de presupuesto-tiempo por frame (fix barato). En T3 el
  processing se reubica al hilo de sim (la mutación de estado va con la sim). _[Prereq: base
  autoritativa reconciliada.]_
- **T3 — Split sim/render (el grande, de-riesgado por los ciclos previos).** SimulationClock +
  AI + movimiento + packets → hilo worker; main = interp + GL + present. Frontera = **snapshot
  triple-buffer** del estado render-read, índice atómico (render nunca bloquea sim). Apalanca
  Stages 1b-4b (ya identificaron la superficie render-read = lo que interpolan). Auditoría previa
  (agente) de todo lo que `RenderScene`/`RenderCharacters` lee de globales para cerrar races.
  Reconciliación autoritativa: el snapshot integra el estado entrante del server SIN pisarlo
  (cierra monster-desync). Arreglar el teardown race (`Connection.cpp:68`) acá: join ordenado del
  worker antes del `disconnect` .NET. _[Prereq: base autoritativa reconciliada.]_
- **T4 — Job-system culling/skinning.** Pool de workers; fan-out de bone-transforms (por char,
  embarrassingly parallel) + culling; join antes de submit. Puente a Fase A (GPU skinning); el
  skinning CPU jobificado queda como fallback.

### Riesgos Fase B
Globals single-thread en todo el engine ⇒ data races. Mitigar: empezar por I/O (aislado), cap de
paquetes, luego snapshot; auditar lectores + assert single-writer en debug. Afinidad de contexto
GL ⇒ todo GL en main, uploads encolados. Determinismo: el server es autoritativo ⇒ el drift de la
predicción lo corrige el server. Medir que no haya tearing/stutter nuevo. Debug infla 5-10× ⇒
medir Release en hitos.

---

## Orden de ejecución (ciclos del loop)

**FASE A (techo FPS, EN CURSO):** A0–A5 ✓ → **chrome instancing (en progreso)** → A6 skip-skinning
CPU → GPU skinning total (matar skin CPU) → A7 medir + ampliar variantes.

**FASE B (estabilidad, DESPUÉS):** T0 instrumentación → T1 I/O async →
**[reconciliar base autoritativa: merge main → temporal]** → T2 packets fuera del frame →
T3 split sim/render → T4 job-system.

Cada ciclo: implementar → `cmake --build windows-x86-debug` → cdb-smoke autónomo (auto-enter,
no-crash, gl_log) → commit. Validación de crowd + A/B visual + Release con el harness
(`MU_TEST_CHARS` + `MU_TEST_SHOT` + `[frame]`).

## Estado (2026-06-14)
Orden confirmado por el usuario: **render primero, threading después**. Fase A en curso (chrome
instancing a medias). Fase B detallada (T0–T4) y aprobada; arranca tras cerrar Fase A.
Integración de la base autoritativa del cliente (`main` → `temporal`) **diferida** por decisión
del usuario; es prereq de T2/T3. Permiso dado para auto-enter (saltear login) y levantar con cdb
sin confirmación.
