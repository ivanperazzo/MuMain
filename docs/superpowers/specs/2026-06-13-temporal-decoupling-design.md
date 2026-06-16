# Desacople simulación/render en MuMain — diseño de integración incremental

**Fecha:** 2026-06-13
**Estado:** aprobado para escribir plan de implementación
**Reporte base:** `docs/temporal-architecture-report.html` (v2 corregido)
**Alcance:** cliente C++ MuMain (`I:\MuOnline\MuMain`). No toca OpenMU en esta fase.

---

## 1. Problema

La velocidad de simulación del juego está acoplada al FPS de render. Subir FPS acelera el
juego (speedhack); bajarlo (multitud de jugadores/efectos en pantalla) lo frena. Causa raíz
confirmada en el reporte:

- **No hay fixed timestep.** La simulación corre sólo dentro de `RenderScene`, gateada por
  `CheckRenderNextFrame` (freeze-on-skip confirmado en `Winmain.cpp` MainLoop).
- **Mecanismo ① (lento < 25 FPS):** `FPS_ANIMATION_FACTOR = clamp(REFERENCE_FPS/FPS, 0, 1)`
  con `REFERENCE_FPS = 25.0`. El clamp a 1.0 frena todo lo escalado por debajo de 25 FPS.
- **Mecanismo ② (speedhack > 25 FPS):** sitios que NO escalan correctamente — categorías
  C (conteos enteros / umbral inverso), D (doble integración / compounding), E (sin factor),
  B✗ (decay roto lineal en vez de exponencial).

El movimiento lineal escalado (`* FPS_ANIMATION_FACTOR`, ~1500 sitios cat A) **sí** es
invariante por encima de 25 FPS; el problema no es "falta el factor" en general, sino los
~65 sitios de las categorías rotas más el clamp.

## 2. Objetivo

Desacoplar la simulación del render mediante **fixed timestep + interpolación de render**,
de forma **incremental y verificable**, sin reescritura y sin cambiar el balance del juego.
`FIXED_DT = 40 ms` (= 25 tps) preserva exactamente el ritmo de referencia actual, así que
ninguna constante de gameplay necesita re-tunearse por el cambio de marco.

Prioridad explícita: **arreglar el game loop antes de considerar migrar el engine gráfico.**
Un renderer mejor sin fixed timestep amplifica el speedhack.

## 3. Decisiones tomadas (locked)

| Eje | Decisión |
|-----|----------|
| Granularidad (worktree = iteración de loop) | **Por etapa de migración** (las 9 etapas de §5 del reporte) |
| Fase de investigación previa | **Deep-dive doc por etapa** en `docs/temporal/NN-slug.md`, aprobado antes de tocar código |
| Estrategia de test | **Extraer lógica de timing a funciones puras + doctest** (la extracción ES la mejora) |
| Rol del `/loop` | **Self-paced con gate por iteración** (nunca avanza solo) |

## 4. Forma del pipeline (una iteración del `/loop` = una etapa)

```
1. INVESTIGAR  → escribir deep-dive doc de la etapa (docs/temporal/NN-slug.md)
2. GATE A      → freno; el usuario aprueba el doc antes de tocar código
3. WORKTREE    → branch temporal/NN-slug off del HEAD mergeado de la etapa previa
4. RED         → escribir test(s) doctest que fallan (spec del comportamiento esperado)
5. GREEN       → extraer lógica a puro + implementar hasta verde
6. VERIFY      → cmake build + ctest + (si aplica) criterio E1–E6 plano vs baseline
7. REPORT + GATE B → reportar evidencia; freno para OK del usuario → siguiente etapa
```

Gate doble por etapa: **A** tras investigar (antes de código), **B** tras verificar (antes
de mergear/seguir). El loop es self-paced: jamás avanza de etapa sin OK explícito.

## 5. Work-list: 9 etapas + grafo de dependencias

Tomadas literalmente de §5 del reporte. **Es una cadena con raíz, no un fan-out libre.**

```
0 Instrumentación ─┐ (CSV log + overlay; produce baseline E1/E2)
                   ├─► 1 Scheduler (SimulationClock, FIXED_DT=40ms, factor≡1.0)
                   │        │
                   │        ├─► 2 Movimiento local (cat A → u/tick, snapshot+lerp Hero)
                   │        ├─► 3 Entidades remotas (interp a estado servidor, buffer paquetes)
                   │        ├─► 4 Animaciones (avance lógico en tick, blend con frameMs)
                   │        ├─► 5 Cámara cinemática (cat E → dt real)
                   │        └─► 6 Física de efectos (D re-integrar, C rediseñar, B✗ → pow)
                   │                 │
                   └────────────────►├─► 7 UI / networking visual (formalizar buffer interp)
                                     └─► 8 Limpieza (borrar factor/clamp/REFERENCE_FPS)
```

| # | Etapa | Cambiar primero | Categorías | Verificación |
|---|-------|-----------------|------------|--------------|
| 0 | Instrumentación | overlay + log CSV (§3/§8) | — | baseline E1/E2 capturado |
| 1 | Scheduler | acumulador + FixedUpdate; FIXED_DT=40ms; factor≡1.0 | marco A/B | velocidad idéntica al baseline a todo FPS |
| 2 | Movimiento local | A → u/tick; snapshot+lerp del Hero | A | E1 plano 30↔240; sin jitter |
| 3 | Entidades remotas | interp a estado servidor; buffer de paquetes | A | otros players suaves; menos rubber-band |
| 4 | Animaciones | avance lógico en tick; blend con frameMs | A/B | misma duración 30↔240 |
| 5 | Cámara cinemática | portar travel/zoom (cat E) a dt real | E | E5 plano |
| 6 | Física de efectos | re-integrar proyectiles/pet/cloth; arreglar B✗; rediseñar C | D, C, B✗ | E3/E4/E6 planos |
| 7 | UI / networking visual | UI ya usa delta; formalizar buffer de interp | — | sin regresión |
| 8 | Limpieza | borrar factor/clamp/REFERENCE_FPS | todas | `grep FPS_ANIMATION_FACTOR` → 0 |

**Reglas de orden:**
- **0 va primero siempre.** Sin medición no se verifica ninguna etapa posterior. El CSV log
  funciona en build normal; el overlay ImGui sólo en builds `-mueditor`.
- **1 es el cimiento.** 2/3/4/5/6 dependen del reloj.
- 2, 3, 4 son mutuamente independientes (paralelizables en worktrees hermanas más adelante;
  el loop self-paced las ejecuta en orden de todas formas).
- **8 va último.** El cierre `grep FPS_ANIMATION_FACTOR → 0` sólo es posible cuando todo lo
  demás migró.

## 6. Modelo de worktree (línea de integración única, secuencial)

Decisión: por la amplitud del trabajo y la cadena de dependencias (cada etapa pisa el reloj
de la anterior), se integra **secuencialmente sobre una línea única**, no en worktrees
paralelas. El loop self-paced ya impone orden; esto evita divergencia y conflictos de merge.

- `I:\MuOnline` **no** es repo git; **`MuMain` sí**. La worktree sale del repo `MuMain`.
- **Una worktree dedicada** (directorio aparte, p.ej. `I:\MuOnline\MuMain-temporal`,
  ruta final al crearla) checkouteada en el branch integrador `temporal/integration`
  (creado del HEAD actual de `MuMain`). Es **necesaria, no opcional**: el usuario está
  trabajando en otras cosas en el árbol actual de `MuMain`, así que todo el trabajo temporal
  ocurre exclusivamente en esta worktree y no interfiere con esa tarea en curso.
- **Un commit por etapa** sobre `temporal/integration` + tag `temporal/stage-NN` =
  puntos de rollback limpios (revertir = volver al tag de la etapa previa).
- Etapa cerrada (Gate B + OK del usuario) → la siguiente etapa continúa sobre el mismo
  branch. Sin merges cruzados entre etapas.
- Al cerrar toda la migración, `temporal/integration` → merge al branch principal (lo
  decide el usuario).
- Push a upstream bloqueado (origin = fork `ivanperazzo`, upstream = original con push
  bloqueado). Los branches viven en el fork / local.

## 7. Modelo de investigación (deep-dive doc por etapa)

Template fijo en `docs/temporal/NN-slug.md`. Es el artefacto que se aprueba en el Gate A.
Sin doc aprobado, el loop no toca código.

```
# Etapa NN — <nombre>
## Qué cambia exactamente      (1 párrafo, específico de MuMain, sin teoría genérica)
## Sitios afectados            (tabla file:line + categoría A/B/C/D/E/B✗)
## Comportamiento actual → objetivo
## Lógica a extraer a puro     (módulo nuevo, firma de función)
## Plan de test                (casos doctest concretos: invariancia 30↔240)
## Riesgo + mitigación         (re-tuneo cat D, overflow cat C, frame counters, etc.)
## Criterio de éxito           (cuál de E1–E6 debe quedar plano)
## Rollback                    (cómo revertir si rompe el feel)
```

## 8. Modelo de test (extraer a puro + doctest)

La restricción del harness existente —los tests linkean **sólo el `.cpp` bajo prueba**, ver
`tests/text/CMakeLists.txt` que compila `TextLineWrap.cpp` aislado— es la palanca del diseño:
**para testear hay que extraer la lógica de timing fuera del engine, y esa extracción es
exactamente la mejora.**

- Módulo puro raíz: `src/source/Core/Time/SimulationClock.{h,cpp}` (acumulador, step,
  `MAX_STEPS`, `alpha`) — cero dependencias de engine. Reemplaza el rol de `CalcFPS`.
- Test `tests/time/test_simulation_clock.cpp`: alimentar secuencia de `frameMs`, assertear
  número de steps, leftover, `alpha ∈ [0,1)`, clamp `MAX_STEPS`, corte de spiral-of-death.
- Por etapa que toca cálculo, extraer su núcleo a puro y assertear **invariancia frame-rate**:
  - cat A: `IntegratePosition(pos, vel, dt)` → misma distancia a 30 y 240 FPS.
  - cat B✗: `DecayLight(x, dt)` → forma `pow`/exponencial (no lineal `1 - factor`).
  - cat C: `EffectTickCount(dt)` → tiempos/ticks, sin overflow (cuidar BYTE de GOBoid).
  - cat D: re-integración determinista de proyectiles/pet/cloth → mismo alcance/arco.
- Cada módulo de test: subdir bajo `tests/` (`tests/time/`, `tests/motion/`, `tests/effects/`)
  con su `CMakeLists.txt` llamando `mu_add_test(NAME ... SOURCES ... LINK_LIBS ...)`. Framework
  doctest (`TEST_CASE` / `CHECK`).
- **Cierre (etapa 8 o módulo dedicado):** harness E1–E6 automatizado = regression test de
  velocidad. Métrica clave `hero_units_per_sec` plano ±2% a 30/60/144/240 FPS.

### Spec de medición / repro (E1–E6, del reporte §3)

| ID | Mide | Plano cuando |
|----|------|-------------|
| E1 | distancia/seg del Hero a 30/60/144/240 FPS | ±2% (cat A invariante) |
| E2 | velocidad de sim con cap a 10 FPS | mantiene velocidad (clamp eliminado) |
| E3 | alcance/arco de proyectiles a 30 vs 240 | igual (cat D resuelta) |
| E4 | densidad de partículas/colas a todo FPS | idéntica (cat C resuelta) |
| E5 | duración de cutscenes/cámara cinemática | igual (cat E resuelta) |
| E6 | duración de fades/decays de luz | igual (cat B✗ resuelta) |

## 9. Definición de "done" por etapa

Una etapa se cierra **sólo si**:
1. Deep-dive doc aprobado (Gate A).
2. Test(s) pasaron de rojo a verde.
3. `cmake --build` sin errores.
4. `ctest` verde.
5. El criterio E# correspondiente queda plano vs baseline.
6. OK del usuario en el Gate B.

Si algo falla, la etapa queda `in_progress` y el loop no avanza.

## 10. Rediseño del game loop (objetivo de la etapa 1)

Pseudocódigo de referencia (del reporte §4):

```cpp
double accumulator = 0.0, prevMs = Timer::NowMs();
const double FIXED_DT_MS = 40.0;     // 25 tps — invariante de balance
const int    MAX_STEPS   = 5;        // corta spiral of death

for (;;) {
    const double nowMs = Timer::NowMs();
    double frameMs = nowMs - prevMs;
    prevMs = nowMs;
    if (frameMs > 250.0) frameMs = 250.0;   // clamp de stall

    accumulator += frameMs;
    int steps = 0;
    while (accumulator >= FIXED_DT_MS && steps < MAX_STEPS) {
        SampleInput();
        FixedUpdate(FIXED_DT_MS);            // sim a paso fijo
        accumulator -= FIXED_DT_MS; ++steps;
    }
    if (steps == MAX_STEPS) accumulator = 0.0;   // descartar deuda

    const float alpha = float(accumulator / FIXED_DT_MS);   // 0..1
    RenderScene(alpha, frameMs);             // render interpolado
}
```

Etapa 1 introduce este loop con `FPS_ANIMATION_FACTOR ≡ 1.0` debajo del código actual: la
sim corre a paso fijo pero las fórmulas viejas siguen usando factor=1.0 → comportamiento
idéntico al baseline a todo FPS, sin migrar aún cada subsistema. Las etapas 2+ migran
subsistema por subsistema al modelo snapshot+lerp y eliminan el factor de cada uno.

## 11. Riesgos técnicos (del reporte §6)

- **Animaciones que asumen 25 FPS:** usar `FIXED_DT=40ms` evita re-tunear.
- **Cat D (re-integración):** arcos de proyectiles cambiarán al pasar a dt fijo — re-tunear
  constantes para igualar el feel a 25 FPS; testear alcance/altura.
- **Cat C (rediseño):** convertir conteos enteros `k/factor` en tiempos/ticks; cuidar el
  overflow ya existente (BYTE de GOBoid).
- **Frame counters absolutos** (`iFrame`, seeds de GOBoid): mapear a tick count o `WorldTime`.
- **Interpolación / rubber-band:** buffer de 1–2 ticks de retraso, interpolar (no extrapolar).
- **Desync con servidor:** OpenMU es autoridad; la interp es cosmética, snap suave si la
  divergencia supera un umbral.
- **Spiral of death:** `MAX_STEPS` + clamp de `frameMs` lo cortan.

## 12. Artefactos

```
docs/superpowers/specs/2026-06-13-temporal-decoupling-design.md   (este diseño)
docs/temporal/00..08-*.md                                          (deep-dives por etapa)
docs/temporal-architecture.md                                      (entregable #1: diseño + invariante 40ms + 6 cats)
src/source/Core/Time/SimulationClock.{h,cpp}                       (etapa 1)
src/source/Render/Interpolation.{h,cpp}                            (snapshot prev/cur + lerp; RenderScene(alpha,frameMs))
tests/time/, tests/motion/, tests/effects/                         (módulos de test por etapa)
```

Las tasks nativas espejan el grafo de dependencias de §5 con `blockedBy`
(0 ← 1 ← {2,3,4,5,6} ← 7 ← 8).

## 13. Fuera de alcance (YAGNI)

- Migración o modernización del engine gráfico (OpenGL 3.3+, RHI, swap de engine). Sólo
  después de un loop sano, y sólo al atacar el objetivo de gráficos.
- Cambios en OpenMU (servidor). Esta fase es 100% cliente.
- Server-authority de cálculos (mover lógica cliente→servidor). Es el objetivo macro del
  monorepo pero un eje aparte; este diseño lo deja deliberadamente fuera.

## 14. Criterios de éxito globales (del reporte §9)

1. Hero recorre misma distancia/seg ±2% a 30/60/144/240 FPS (E1).
2. Cap a 10 FPS: simulación mantiene velocidad (no el ~40% actual) — clamp eliminado (E2).
3. Render fluido a 144 FPS con sim a 25 tps por interpolación (sin stutter de 25 Hz).
4. Sin spiral of death: stall de 1s no produce avance acelerado posterior.
5. Proyectiles: mismo alcance/arco a 30 y 240 (E3).
6. Densidad de partículas/colas idéntica a todo FPS (E4).
7. Cutscenes y fades misma duración a todo FPS (E5/E6).
8. `grep FPS_ANIMATION_FACTOR` → 0 resultados al cerrar la migración.
