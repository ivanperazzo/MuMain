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

Objetivo: que stalls de sim/red/IO NO tiren el frame, y que el render corra a su ritmo. GL debe
seguir en UN hilo (el dueño del contexto). Modelo objetivo (estilo render-thread):

- **Hilo de render**: dueño del contexto GL. Cada frame lee un SNAPSHOT inmutable del estado de
  juego (double/triple buffer, swap lock-free) y hace todo el dibujo + present. La interpolación
  (Stages 3/4) ya lee snapshots prev/cur ⇒ encaja.
- **Hilo principal/sim**: input + ventana (SDL exige eventos de ventana en el main en algunas
  plataformas) + sim fixed 25tps + red; publica snapshots.
- **IO/assets**: carga de texturas/modelos en hilo aparte; el render usa placeholder hasta que
  estén listas (elimina hitches de carga — probablemente el win de estabilidad más grande y el
  MENOS riesgoso ⇒ primer sub-paso de Fase B).

### Ciclos de Fase B (de menor a mayor riesgo)
- **B0 — Aislar IO/carga de assets** a un hilo worker (low-risk; mata hitches de streaming).
- **B1 — Doble-buffer de estado de render** (snapshot inmutable que el render consume). Formaliza
  lo de Stage 3/4.
- **B2 — Mover render+present a un hilo dedicado** con el contexto GL; main hace input/ventana/sim.
  Alto riesgo (globals single-thread por todos lados). Puede quedar como meta final / opcional.

### Riesgos Fase B
El engine asume globals single-thread en todos lados ⇒ data races. Mitigar: empezar por IO
(aislado), luego snapshot, y el render-thread completo solo si Fase A no alcanzó la meta. SDL +
contexto GL: `SDL_GL_MakeCurrent` en el hilo de render; eventos de ventana en main. Medir que no
haya tearing/stutter nuevo.

---

## Orden de ejecución (ciclos del loop)

A0 → A1 → A2 → A3 → A4 → A5 → A6 → A7 → (medir; si techo insuficiente y/o hay hitches) → B0 → B1 → B2.

Cada ciclo: implementar → `cmake --build windows-x86-debug` → cdb-smoke autónomo (auto-enter,
no-crash, gl_log) → commit. Validación de crowd + A/B visual + Release: con el usuario presente.

## Estado
Diseño aprobado por el usuario (ambos en orden) para ejecución autónoma en loop. Permiso dado para
auto-enter (saltear login) y levantar con cdb sin confirmación. Arranca A0.
