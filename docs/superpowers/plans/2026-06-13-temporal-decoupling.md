# Desacople simulación/render en MuMain — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Desacoplar la simulación del render en el cliente MuMain mediante fixed timestep + interpolación, de forma incremental, verificable y respaldada por tests, sin cambiar el balance del juego.

**Architecture:** Una worktree dedicada (`MuMain-temporal`) sobre el branch `temporal/integration`. Se avanza secuencialmente etapa por etapa (cadena con raíz: instrumentación → reloj fixed-step → migración de subsistemas → limpieza). Cada etapa: deep-dive doc → gate → tests rojo→verde → build/ctest → criterio E# plano → gate. La lógica de timing se extrae a funciones/clases puras en `Core/` (cero deps de engine) para hacerla testeable con doctest; esa extracción ES la mejora.

**Tech Stack:** C++20, CMake + Ninja Multi-Config, doctest (`tests/`), SDL3 + OpenGL. Reloj de pared existente: `g_pTimer->GetTimeElapsed()` (CTimer, ms double) y `Core::Time::FrameTimerScheduler::NowMs()` (uint64 ms).

**Spec base:** `docs/superpowers/specs/2026-06-13-temporal-decoupling-design.md` · **Reporte:** `docs/temporal-architecture-report.html`

---

## Protocolo del loop (cómo se ejecuta CADA tarea)

Toda tarea de etapa sigue el mismo ciclo self-paced con doble gate:

1. **Investigar** → escribir/actualizar el deep-dive doc `docs/temporal/NN-slug.md` (template abajo).
2. **GATE A** → parar; el usuario aprueba el doc antes de tocar código.
3. **Red** → escribir el/los test doctest que fallan (spec ejecutable del comportamiento).
4. **Green** → extraer lógica a puro + implementar contra los sitios enumerados en el doc, hasta verde.
5. **Verify** → `cmake --build` + `ctest`; y el criterio E# correspondiente plano vs baseline.
6. **GATE B** → reportar evidencia; el usuario da OK; commit + tag `temporal/stage-NN`; recién entonces la siguiente etapa.

### Template del deep-dive doc (`docs/temporal/NN-slug.md`)

```
# Etapa NN — <nombre>
## Qué cambia exactamente      (1 párrafo, específico de MuMain, sin teoría genérica)
## Sitios afectados            (tabla file:line + categoría A/B/C/D/E/B✗)
## Comportamiento actual → objetivo
## Lógica a extraer a puro     (módulo nuevo, firma de función)
## Plan de test                (casos doctest concretos: invariancia 30↔240)
## Riesgo + mitigación
## Criterio de éxito           (cuál de E1–E6 debe quedar plano)
## Rollback
```

### Comandos canónicos (worktree `MuMain-temporal`)

```bash
# Configurar con tests habilitados (una vez)
cmake --preset windows-x86 -DBUILD_TESTING=ON

# Build del juego
cmake --build --preset windows-x86-debug

# Build + correr SOLO los tests (Ninja Multi-Config => -C Debug)
cmake --build --preset windows-x86-debug --target all
ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure
```

### Criterios E (spec de medición, del reporte §3)

| ID | Mide | Plano cuando |
|----|------|-------------|
| E1 | distancia/seg del Hero a 30/60/144/240 FPS | ±2% |
| E2 | velocidad de sim con cap a 10 FPS | mantiene velocidad (clamp eliminado) |
| E3 | alcance/arco de proyectiles a 30 vs 240 | igual |
| E4 | densidad de partículas/colas a todo FPS | idéntica |
| E5 | duración de cutscenes/cámara cinemática | igual |
| E6 | duración de fades/decays de luz | igual |

---

## Grafo de dependencias de tareas

```
Task 1 (Stage 0: instrumentación) ─┐
Task 2 (Stage 1: SimulationClock) ─┼─► Task 3 (Stage 1b: wire MainLoop)
                                    │        └─► Task 4 (Stage 2: movimiento)
                                    │        └─► Task 5 (Stage 3: remotas)
                                    │        └─► Task 6 (Stage 4: animaciones)
                                    │        └─► Task 7 (Stage 5: cámara cinemática)
                                    │        └─► Task 8 (Stage 6: física efectos)
                                    └────────────────► (todas) ─► Task 9 (Stage 7: UI) ─► Task 10 (Stage 8: limpieza)
```

---

## Task 1: Stage 0 — Instrumentación (probe de movimiento + log CSV)

**Goal:** Capturar el baseline medible (`hero_units_per_sec` y frame timing a distintos FPS) para verificar todas las etapas siguientes, con la matemática del agregado extraída a un módulo puro testeable.

**Files:**
- Create: `src/source/Core/Diagnostics/MovementProbe.h`
- Create: `src/source/Core/Diagnostics/MovementProbe.cpp`
- Create: `tests/diagnostics/CMakeLists.txt`
- Create: `tests/diagnostics/test_movement_probe.cpp`
- Modify: `tests/CMakeLists.txt` (agregar `add_subdirectory(diagnostics)`)
- Modify: `src/source/Scenes/SceneManager.cpp` (llamar al probe + volcar fila CSV por frame, detrás de un flag)

**Acceptance Criteria:**
- [ ] `MovementProbe` calcula `units_per_sec` a partir de muestras (posición, tiempoMs) de forma frame-rate independiente (mismo resultado con muchas muestras chicas que con pocas grandes, ±epsilon).
- [ ] El CSV registra por frame: `t_ms,fps,hero_x,hero_y,hero_units_per_sec,steps,interp_alpha` (las dos últimas quedan en 0 hasta la Stage 1).
- [ ] Logging detrás de flag (env var `MU_TEMPORAL_CSV` o constante) — apagado por defecto, sin costo en build normal.
- [ ] Baseline capturado a 30/60/144 FPS y guardado en `docs/temporal/baseline/` (CSV adjuntos).

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R movement_probe --output-on-failure` → todos los TEST_CASE PASS.

**Steps:**

- [ ] **Step 1: Escribir el test que falla** — `tests/diagnostics/test_movement_probe.cpp`

```cpp
#include "doctest.h"
#include "Core/Diagnostics/MovementProbe.h"

using Core::Diagnostics::MovementProbe;

TEST_CASE("units_per_sec is frame-rate independent")
{
    // 100 unidades en 1000 ms = 100 u/s, sin importar el número de muestras.
    MovementProbe coarse;   // 25 muestras de 40 ms
    for (int i = 1; i <= 25; ++i)
        coarse.Sample(/*x*/ i * 4.0f, /*y*/ 0.0f, /*tMs*/ i * 40.0);

    MovementProbe fine;     // 250 muestras de 4 ms, misma trayectoria total
    for (int i = 1; i <= 250; ++i)
        fine.Sample(/*x*/ i * 0.4f, /*y*/ 0.0f, /*tMs*/ i * 4.0);

    CHECK(coarse.UnitsPerSec() == doctest::Approx(100.0).epsilon(0.01));
    CHECK(fine.UnitsPerSec()   == doctest::Approx(100.0).epsilon(0.01));
}

TEST_CASE("no movement => zero units_per_sec")
{
    MovementProbe p;
    p.Sample(10.0f, 10.0f, 40.0);
    p.Sample(10.0f, 10.0f, 80.0);
    CHECK(p.UnitsPerSec() == doctest::Approx(0.0));
}

TEST_CASE("first sample establishes origin, reports zero")
{
    MovementProbe p;
    p.Sample(5.0f, 5.0f, 0.0);
    CHECK(p.UnitsPerSec() == doctest::Approx(0.0));
}
```

- [ ] **Step 2: Correr el test, verificar que falla** (no compila: `MovementProbe` no existe).

Run: `ctest --test-dir out/build/windows-x86 -C Debug -R movement_probe`
Expected: FAIL (compile error / missing header).

- [ ] **Step 3: Implementar el módulo puro** — `src/source/Core/Diagnostics/MovementProbe.h`

```cpp
#pragma once

namespace Core::Diagnostics
{
    // Pure rolling probe: feed (x, y, timeMs) samples, get distance/sec over the
    // window. No engine deps -> unit-testable. Frame-rate independent because it
    // divides accumulated distance by accumulated wall time, not by frame count.
    class MovementProbe
    {
    public:
        void  Sample(float x, float y, double timeMs);
        double UnitsPerSec() const;   // 0 until at least two samples exist
        void  Reset();

    private:
        bool   m_hasPrev = false;
        float  m_prevX = 0.0f, m_prevY = 0.0f;
        double m_firstMs = 0.0, m_lastMs = 0.0;
        double m_distance = 0.0;
    };
}
```

`src/source/Core/Diagnostics/MovementProbe.cpp`

```cpp
#include "Core/Diagnostics/MovementProbe.h"
#include <cmath>

namespace Core::Diagnostics
{
    void MovementProbe::Sample(float x, float y, double timeMs)
    {
        if (!m_hasPrev)
        {
            m_hasPrev = true;
            m_prevX = x; m_prevY = y;
            m_firstMs = m_lastMs = timeMs;
            return;
        }
        const double dx = static_cast<double>(x) - m_prevX;
        const double dy = static_cast<double>(y) - m_prevY;
        m_distance += std::sqrt(dx * dx + dy * dy);
        m_prevX = x; m_prevY = y;
        m_lastMs = timeMs;
    }

    double MovementProbe::UnitsPerSec() const
    {
        const double elapsedMs = m_lastMs - m_firstMs;
        if (elapsedMs <= 0.0) return 0.0;
        return m_distance / (elapsedMs / 1000.0);
    }

    void MovementProbe::Reset()
    {
        m_hasPrev = false;
        m_distance = 0.0;
        m_firstMs = m_lastMs = 0.0;
    }
}
```

- [ ] **Step 4: Registrar el módulo de test** — `tests/diagnostics/CMakeLists.txt`

```cmake
mu_add_test(
    NAME test_movement_probe
    SOURCES
        test_movement_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Diagnostics/MovementProbe.cpp
)
```

y en `tests/CMakeLists.txt`, junto a los otros `add_subdirectory`:

```cmake
add_subdirectory(diagnostics)
```

- [ ] **Step 5: Correr el test, verificar verde.**

Run: `cmake --build --preset windows-x86-debug && ctest --test-dir out/build/windows-x86 -C Debug -R movement_probe --output-on-failure`
Expected: PASS (3 TEST_CASE).

- [ ] **Step 6: Cablear el probe + CSV en `SceneManager.cpp`** detrás de flag (apagado por defecto). En `RenderScene`/`CalcFPS` muestrear la posición del Hero y `WorldTime`, y escribir la fila CSV. Mantener el costo en cero cuando el flag está off (chequeo de bool al frente).

- [ ] **Step 7: Capturar baseline** corriendo el juego a 30/60/144 FPS (`SetTargetFps`), moviendo al Hero una distancia fija; guardar los CSV en `docs/temporal/baseline/`.

- [ ] **Step 8: Commit + tag.**

```bash
git add src/source/Core/Diagnostics/ tests/diagnostics/ tests/CMakeLists.txt src/source/Scenes/SceneManager.cpp docs/temporal/
git commit -m "feat(temporal): stage 0 instrumentation - movement probe + CSV baseline"
git tag temporal/stage-00
```

---

## Task 2: Stage 1 — SimulationClock (acumulador fixed-step puro)

**Goal:** Introducir el núcleo del fixed timestep como una clase pura, sin deps de engine, totalmente cubierta por tests de invariancia frame-rate. Reemplaza el rol de cálculo temporal de `CalcFPS`.

**Files:**
- Create: `src/source/Core/Time/SimulationClock.h`
- Create: `src/source/Core/Time/SimulationClock.cpp`
- Create: `tests/time/CMakeLists.txt`
- Create: `tests/time/test_simulation_clock.cpp`
- Modify: `tests/CMakeLists.txt` (`add_subdirectory(time)`)

**Acceptance Criteria:**
- [ ] `Advance(frameMs)` devuelve nº de steps, `alpha ∈ [0,1)`, y flag `droppedDebt`.
- [ ] Invariancia: alimentar 1000 ms en chunks de 8 ms (≈125 FPS) y en chunks de 40 ms (25 FPS) produce el MISMO total de steps (25).
- [ ] `MAX_STEPS` corta la spiral of death: un frame de 1000 ms produce ≤ `maxSteps` steps y `droppedDebt == true`.
- [ ] `frameMs` negativo o > `maxFrameMs` se clampa; sin under/overflow.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R simulation_clock --output-on-failure` → todos PASS.

**Steps:**

- [ ] **Step 1: Escribir el test que falla** — `tests/time/test_simulation_clock.cpp`

```cpp
#include "doctest.h"
#include "Core/Time/SimulationClock.h"

using Core::Time::SimulationClock;
using Core::Time::ClockConfig;

TEST_CASE("one fixed frame yields one step, zero alpha")
{
    SimulationClock clk;                 // default 40ms / maxSteps 5
    auto r = clk.Advance(40.0);
    CHECK(r.steps == 1);
    CHECK(r.alpha == doctest::Approx(0.0f));
    CHECK(r.droppedDebt == false);
}

TEST_CASE("partial frame accumulates into alpha")
{
    SimulationClock clk;
    auto r = clk.Advance(20.0);          // half a tick
    CHECK(r.steps == 0);
    CHECK(r.alpha == doctest::Approx(0.5f));
}

TEST_CASE("100ms frame: 2 steps, 20ms leftover, alpha 0.5")
{
    SimulationClock clk;
    auto r = clk.Advance(100.0);
    CHECK(r.steps == 2);
    CHECK(r.alpha == doctest::Approx(0.5f));
}

TEST_CASE("step count is frame-rate independent over the same wall time")
{
    SimulationClock fast;
    int fastSteps = 0;
    for (int i = 0; i < 125; ++i) fastSteps += fast.Advance(8.0).steps;   // ~125 FPS

    SimulationClock slow;
    int slowSteps = 0;
    for (int i = 0; i < 25; ++i) slowSteps += slow.Advance(40.0).steps;   // 25 FPS

    CHECK(fastSteps == 25);
    CHECK(slowSteps == 25);
}

TEST_CASE("spiral of death is cut by MAX_STEPS")
{
    SimulationClock clk;                 // maxSteps 5, maxFrameMs 250
    auto r = clk.Advance(1000.0);        // clamped to 250 -> 6.25 ticks -> 5 + debt
    CHECK(r.steps == 5);
    CHECK(r.droppedDebt == true);
    CHECK(clk.Accumulator() == doctest::Approx(0.0));
}

TEST_CASE("negative frameMs is guarded")
{
    SimulationClock clk;
    auto r = clk.Advance(-5.0);
    CHECK(r.steps == 0);
    CHECK(r.alpha == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Correr, verificar que falla** (header inexistente).

Run: `ctest --test-dir out/build/windows-x86 -C Debug -R simulation_clock`
Expected: FAIL (compile error).

- [ ] **Step 3: Implementar** — `src/source/Core/Time/SimulationClock.h`

```cpp
#pragma once

namespace Core::Time
{
    struct ClockConfig
    {
        double fixedDtMs  = 40.0;    // 25 tps — invariante de balance (= REFERENCE_FPS actual)
        int    maxSteps   = 5;       // corta la spiral of death
        double maxFrameMs = 250.0;   // clamp de stall antes de acumular
    };

    struct StepResult
    {
        int   steps       = 0;       // cuántos FixedUpdate ejecutar este frame
        float alpha       = 0.0f;    // interpolación de render [0,1)
        bool  droppedDebt = false;   // true si se descartó deuda por maxSteps
    };

    // Pure fixed-timestep accumulator. No engine deps -> unit-testable.
    // Feed the wall-clock frame delta (ms); get back how many fixed sim steps to
    // run this frame and the render interpolation alpha.
    class SimulationClock
    {
    public:
        explicit SimulationClock(ClockConfig cfg = {}) : m_cfg(cfg) {}

        StepResult Advance(double frameMs);

        double FixedDtMs()   const { return m_cfg.fixedDtMs; }
        double Accumulator() const { return m_accumulator; }
        void   Reset()             { m_accumulator = 0.0; }

    private:
        ClockConfig m_cfg;
        double      m_accumulator = 0.0;
    };
}
```

`src/source/Core/Time/SimulationClock.cpp`

```cpp
#include "Core/Time/SimulationClock.h"

namespace Core::Time
{
    StepResult SimulationClock::Advance(double frameMs)
    {
        if (frameMs < 0.0)             frameMs = 0.0;
        if (frameMs > m_cfg.maxFrameMs) frameMs = m_cfg.maxFrameMs;

        m_accumulator += frameMs;

        StepResult r;
        while (m_accumulator >= m_cfg.fixedDtMs && r.steps < m_cfg.maxSteps)
        {
            m_accumulator -= m_cfg.fixedDtMs;
            ++r.steps;
        }
        if (r.steps == m_cfg.maxSteps && m_accumulator >= m_cfg.fixedDtMs)
        {
            m_accumulator = 0.0;       // descartar deuda
            r.droppedDebt = true;
        }
        r.alpha = static_cast<float>(m_accumulator / m_cfg.fixedDtMs);
        return r;
    }
}
```

- [ ] **Step 4: Registrar el test** — `tests/time/CMakeLists.txt`

```cmake
mu_add_test(
    NAME test_simulation_clock
    SOURCES
        test_simulation_clock.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Time/SimulationClock.cpp
)
```

y `add_subdirectory(time)` en `tests/CMakeLists.txt`.

- [ ] **Step 5: Correr, verificar verde.**

Run: `cmake --build --preset windows-x86-debug && ctest --test-dir out/build/windows-x86 -C Debug -R simulation_clock --output-on-failure`
Expected: PASS (6 TEST_CASE).

- [ ] **Step 6: Commit + tag.**

```bash
git add src/source/Core/Time/SimulationClock.* tests/time/ tests/CMakeLists.txt
git commit -m "feat(temporal): stage 1 SimulationClock - pure fixed-timestep accumulator"
git tag temporal/stage-01
```

---

## Task 3: Stage 1b — Cablear SimulationClock en el MainLoop (factor ≡ 1.0)

**Goal:** Hacer correr la simulación a paso fijo dentro del MainLoop usando `SimulationClock`, manteniendo `FPS_ANIMATION_FACTOR ≡ 1.0` para que el comportamiento sea idéntico al baseline a todo FPS (sin migrar aún cada subsistema). Render recibe `alpha`.

**USER-ORDERED GATE — NON-SKIPPABLE.** Este task fue pedido por el usuario en la conversación. No se cierra rodeándolo, ni declarándolo "verificado inline", ni sustituyendo el chequeo por uno más barato. Cerrar sólo tras re-validar cada criterio de aceptación con evidencia capturada (CSV baseline vs post-cambio).

**Files:**
- Modify: `src/source/App/Platform/Windows/Winmain.cpp:980-1152` (`MainLoop`: insertar acumulador + bucle `FixedUpdate`)
- Modify: `src/source/Scenes/SceneManager.cpp` (separar la sim de `RenderScene`; exponer `FixedUpdate(dtMs)` y `RenderScene(alpha)`)
- Modify: `src/source/Scenes/SceneCore.h` / `.cpp` (firmas)
- Modify: `src/source/Engine/AI/ZzzAI.cpp` (`FPS_ANIMATION_FACTOR` forzado a 1.0 durante esta etapa; `CalcFPS` ya no maneja el step temporal)
- Test: `tests/time/test_simulation_clock.cpp` (ya cubre el acumulador; la integración se verifica por E1/E2 empírico vs baseline)

**Acceptance Criteria:**
- [ ] La sim corre dentro de un bucle `while (steps--) FixedUpdate(FIXED_DT)`, no dentro de `RenderScene`.
- [ ] `FPS_ANIMATION_FACTOR == 1.0` en todo momento durante esta etapa (verificable por `grep` + assert en runtime).
- [ ] E1: `hero_units_per_sec` a 30/60/144 FPS coincide con el baseline de Task 1 dentro de ±2%.
- [ ] E2: con cap a 10 FPS la simulación NO cae al ~40% (el clamp dejó de gobernar la velocidad).
- [ ] Sin spiral of death: un stall de 1 s no produce avance acelerado posterior (cubierto por el test `droppedDebt`, confirmado en runtime).
- [ ] `cmake --build` y `ctest` verdes.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure` verde; y comparación CSV `docs/temporal/baseline/` vs nueva captura a 30/60/144 FPS → `hero_units_per_sec` plano ±2% (E1) y velocidad sostenida a 10 FPS (E2).

**Steps:**

- [ ] **Step 1: Deep-dive doc** `docs/temporal/01b-mainloop-wire.md` — enumerar exactamente qué se mueve de `RenderScene` a `FixedUpdate`, qué queda en render, y cómo se fuerza factor≡1.0. GATE A.
- [ ] **Step 2: Insertar el acumulador en `MainLoop`** (referencia §10 del reporte / spec §10). El delta de frame sale de `g_pTimer->GetTimeElapsed()` (ya usado por `CheckRenderNextFrame`):

```cpp
// dentro de MainLoop(), antes del bloque CheckRenderNextFrame:
static Core::Time::SimulationClock s_simClock;
static double s_prevMs = g_pTimer->GetTimeElapsed();
const double nowMs   = g_pTimer->GetTimeElapsed();
const double frameMs = nowMs - s_prevMs;
s_prevMs = nowMs;

const auto step = s_simClock.Advance(frameMs);
for (int i = 0; i < step.steps; ++i)
{
    SampleInput();
    FixedUpdate(s_simClock.FixedDtMs());   // antes: la sim vivía en RenderScene
}
// render con interpolación (alpha sin usar aún hasta Stage 2):
if (CheckRenderNextFrame() && (g_bUseWindowMode || g_bWndActive || g_HasInactiveFpsOverride))
    RenderScene(g_hDC, step.alpha);
```

- [ ] **Step 3: Separar sim de render en `SceneManager.cpp`** — extraer las actualizaciones de mundo de `MainScene`/`RenderScene` a `FixedUpdate(dtMs)`; dejar en `RenderScene(hDC, alpha)` sólo dibujo. Forzar `FPS_ANIMATION_FACTOR = 1.0f`.
- [ ] **Step 4: Build + ctest** verdes (el acumulador ya está testeado en Task 2).
- [ ] **Step 5: Verificación empírica E1/E2** — recapturar CSV a 30/60/144 y 10 FPS; comparar con baseline. Adjuntar a `docs/temporal/01b-results.md`.
- [ ] **Step 6: GATE B + commit + tag** `temporal/stage-01b`.

```json:metadata
{"files": ["src/source/App/Platform/Windows/Winmain.cpp", "src/source/Scenes/SceneManager.cpp", "src/source/Scenes/SceneCore.h", "src/source/Engine/AI/ZzzAI.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure", "acceptanceCriteria": ["sim corre en FixedUpdate fuera de RenderScene", "FPS_ANIMATION_FACTOR == 1.0", "E1 hero_units_per_sec plano +/-2% vs baseline a 30/60/144", "E2 velocidad sostenida a 10 FPS", "ctest verde"], "userGate": true, "tags": ["user-gate"], "requireEvidenceTokens": [["baseline"], ["post-cambio"]]}
```

---

## Tasks 4–10: Etapas de migración (investigación-gated)

Cada una sigue el **Protocolo del loop** de arriba. La implementación concreta de cada etapa se deriva de su deep-dive doc aprobado (Gate A) — por eso los pasos de código no se pre-escriben aquí: hacerlo sería adivinar antes de la investigación que el usuario pidió explícitamente. Lo que SÍ es concreto y fijo: el deliverable (deep-dive doc + tests), los sitios objetivo (del reporte), y el criterio de aceptación (E#).

### Task 4: Stage 2 — Movimiento local (cat A → unidades por tick + snapshot/lerp Hero)

**USER-ORDERED GATE — NON-SKIPPABLE.** Pedido por el usuario; cerrar sólo con E1 re-validado y evidencia CSV capturada.

**Goal:** Migrar el movimiento del Hero a unidades por tick (no por frame) y renderizar con interpolación snapshot prev/cur (`alpha`).

**Files:**
- Create: `docs/temporal/02-movimiento-local.md` (deep-dive)
- Create: `src/source/Render/Interpolation.h` / `.cpp` (snapshot prev/cur + `lerp`)
- Create: `tests/motion/CMakeLists.txt`, `tests/motion/test_motion_integration.cpp`
- Modify: `src/source/Engine/Object/ZzzCharacter.cpp` (`MoveCharacterPosition` ~6363/6371; `CharacterMoveSpeed` ~6276)
- Modify: `tests/CMakeLists.txt`

**Acceptance Criteria:**
- [ ] Función pura de integración (`IntegratePosition(pos, vel, dtMs)`) extraída y testeada: misma distancia a 30 y 240 FPS (±epsilon).
- [ ] Hero renderizado con interpolación: sin jitter a 144 FPS, sin stutter de 25 Hz.
- [ ] E1 plano 30↔240 FPS (±2%) usando el CSV de Task 1.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R motion --output-on-failure` verde; E1 plano vs baseline.

```json:metadata
{"files": ["src/source/Render/Interpolation.h", "src/source/Render/Interpolation.cpp", "src/source/Engine/Object/ZzzCharacter.cpp", "tests/motion/test_motion_integration.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug -R motion --output-on-failure", "acceptanceCriteria": ["IntegratePosition pura testeada invariante 30 vs 240", "Hero interpolado sin jitter", "E1 plano +/-2%", "ctest verde"], "userGate": true, "tags": ["user-gate"], "requireEvidenceTokens": [["baseline","30fps"], ["240fps"]]}
```

### Task 5: Stage 3 — Entidades remotas (interpolación a estado servidor + buffer de paquetes)

**Goal:** Interpolar otros jugadores/mobs hacia el estado del servidor con buffer de 1–2 ticks; reducir rubber-band.

**Files:**
- Create: `docs/temporal/03-entidades-remotas.md`
- Modify: red/entidades (derivar del deep-dive; candidatos `WSclient.cpp`, `ZzzCharacter.cpp`)
- Create: `tests/motion/test_remote_interp.cpp`

**Acceptance Criteria:**
- [ ] Buffer de interpolación de red (estado servidor → 1–2 ticks) con función pura de interpolación testeada (no extrapola).
- [ ] Otros players se mueven suaves; menos rubber-band observable.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R remote_interp --output-on-failure` verde.

```json:metadata
{"files": ["docs/temporal/03-entidades-remotas.md", "tests/motion/test_remote_interp.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug -R remote_interp --output-on-failure", "acceptanceCriteria": ["buffer interp red testeado (no extrapola)", "remotas suaves", "ctest verde"], "tags": []}
```

### Task 6: Stage 4 — Animaciones (avance lógico en tick + blend con frameMs)

**Goal:** Avanzar el frame de animación en la sim (tick) y mezclar visualmente con `frameMs`; misma duración a todo FPS.

**Files:**
- Create: `docs/temporal/04-animaciones.md`
- Modify: `src/source/Render/Models/ZzzBMD.cpp:397` (`BMD::PlayAnimation`, driver de `AnimationFrame`)
- Create: `tests/anim/CMakeLists.txt`, `tests/anim/test_anim_advance.cpp`

**Acceptance Criteria:**
- [ ] Función pura de avance de animación testeada: misma duración total (frames acumulados) a 30 y 240 FPS.
- [ ] Sin "doble velocidad" de animaciones a alto FPS.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R anim --output-on-failure` verde.

```json:metadata
{"files": ["src/source/Render/Models/ZzzBMD.cpp", "tests/anim/test_anim_advance.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug -R anim --output-on-failure", "acceptanceCriteria": ["avance animacion puro testeado invariante", "ctest verde"], "tags": []}
```

### Task 7: Stage 5 — Cámara cinemática (cat E → dt real)

**USER-ORDERED GATE — NON-SKIPPABLE.** Pedido por el usuario; cerrar sólo con E5 re-validado y evidencia capturada.

**Goal:** Portar el travel/zoom de la cámara cinemática (sitios cat E sin factor) a dt real, para que las cutscenes duren lo mismo a todo FPS.

**Files:**
- Create: `docs/temporal/05-camara-cinematica.md`
- Modify: `src/source/Camera/CameraMove.cpp` (travel 388-389, zoom 402-407, 428-429)
- Create: `tests/camera/CMakeLists.txt`, `tests/camera/test_camera_travel.cpp`

**Acceptance Criteria:**
- [ ] Travel/zoom de cámara extraídos a función pura por-dt y testeados: mismo desplazamiento total a 30 y 240 FPS.
- [ ] E5: duración de cutscene idéntica a todo FPS.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R camera --output-on-failure` verde; E5 plano.

```json:metadata
{"files": ["src/source/Camera/CameraMove.cpp", "tests/camera/test_camera_travel.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug -R camera --output-on-failure", "acceptanceCriteria": ["travel/zoom puro por-dt testeado invariante", "E5 duracion cutscene igual a todo FPS", "ctest verde"], "userGate": true, "tags": ["user-gate"], "requireEvidenceTokens": [["30fps"], ["240fps"]]}
```

### Task 8: Stage 6 — Física de efectos (cat D re-integrar, C rediseñar, B✗ → pow)

**USER-ORDERED GATE — NON-SKIPPABLE.** Pedido por el usuario; cerrar sólo con E3/E4/E6 re-validados y evidencia capturada.

**Goal:** Arreglar las categorías genuinamente rotas que aceleran/densifican sobre 25 FPS: re-integrar proyectiles/pet/cloth (D) con dt fijo, rediseñar conteos enteros (C) a tiempos/ticks sin overflow, y corregir decays rotos (B✗) a forma `pow`/exponencial.

**Files:**
- Create: `docs/temporal/06-fisica-efectos.md`
- Modify: `src/source/Engine/Object/ZzzEffect.cpp:13130-13184` (cat D misiles), `ZzzEffectParticle.cpp:3957-3964` (cat B✗ decay), `ZzzEffectJoint.cpp:2709` (cat C tail density), `CSPetSystem.cpp:511-530` (cat D pet jump), `PhysicsManager.cpp:76-106` (cat D cloth), `GOBoid.cpp:735/749/763` (cat C/E counter)
- Create: `tests/effects/CMakeLists.txt`, `tests/effects/test_effect_physics.cpp`

**Acceptance Criteria:**
- [ ] cat B✗: `DecayLight(x, dtMs)` puro usa forma exponencial (`pow`), testeado invariante 30↔240.
- [ ] cat C: conteos (`EffectTickCount`, tail density) convertidos a tiempos/ticks, sin overflow (cubrir el BYTE de GOBoid).
- [ ] cat D: re-integración determinista de proyectiles/pet/cloth — mismo alcance/arco a 30 y 240 (re-tunear constantes para igualar feel a 25 FPS).
- [ ] E3 (alcance proyectiles), E4 (densidad partículas/colas), E6 (fades) planos.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug -R effect --output-on-failure` verde; E3/E4/E6 planos.

```json:metadata
{"files": ["src/source/Engine/Object/ZzzEffect.cpp", "src/source/Engine/Object/ZzzEffectParticle.cpp", "src/source/Engine/Object/ZzzEffectJoint.cpp", "src/source/GameLogic/Pets/CSPetSystem.cpp", "src/source/Engine/AI/GOBoid.cpp", "tests/effects/test_effect_physics.cpp"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug -R effect --output-on-failure", "acceptanceCriteria": ["B-roto decay pow puro testeado", "cat C conteos a ticks sin overflow", "cat D re-integracion mismo alcance 30 vs 240", "E3/E4/E6 planos", "ctest verde"], "userGate": true, "tags": ["user-gate"], "requireEvidenceTokens": [["30fps","baseline"], ["240fps"]]}
```

### Task 9: Stage 7 — UI / networking visual (formalizar buffer de interpolación)

**Goal:** La UI ya usa delta; formalizar el buffer de interpolación de red como API estable; verificar sin regresión.

**Files:**
- Create: `docs/temporal/07-ui-networking.md`
- Modify: capa UI / `Render/Interpolation` (derivar del deep-dive)

**Acceptance Criteria:**
- [ ] Buffer de interpolación expuesto como API limpia y documentada.
- [ ] Sin regresión visible en UI ni en suavidad de entidades.
- [ ] `ctest` verde.

**Verify:** `ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure` verde; revisión visual sin regresión.

```json:metadata
{"files": ["docs/temporal/07-ui-networking.md"], "verifyCommand": "ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure", "acceptanceCriteria": ["API buffer interp estable", "sin regresion UI", "ctest verde"], "tags": []}
```

### Task 10: Stage 8 — Limpieza (borrar factor/clamp/REFERENCE_FPS)

**USER-ORDERED GATE — NON-SKIPPABLE.** Pedido por el usuario; cerrar sólo con `grep` re-ejecutado y salida capturada.

**Goal:** Eliminar `FPS_ANIMATION_FACTOR`, el clamp y `REFERENCE_FPS`; cerrar la migración.

**Files:**
- Create: `docs/temporal/08-limpieza.md`
- Modify: `src/source/Engine/AI/ZzzAI.cpp` (`CalcFPS`, `FPS_ANIMATION_FACTOR`, `REFERENCE_FPS`) + todos los call-sites residuales
- Create: `tests/regression/CMakeLists.txt`, `tests/regression/test_speed_invariance.cpp` (E1–E6 automatizados sobre las funciones puras)

**Acceptance Criteria:**
- [ ] `grep -r FPS_ANIMATION_FACTOR src/` → 0 resultados.
- [ ] `grep -r REFERENCE_FPS src/` → 0 resultados.
- [ ] Suite de regresión E1–E6 (sobre las funciones puras extraídas) verde.
- [ ] `cmake --build` + `ctest` verdes.

**Verify:** `grep -rn "FPS_ANIMATION_FACTOR\|REFERENCE_FPS" src/source` → vacío; `ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure` verde.

```json:metadata
{"files": ["src/source/Engine/AI/ZzzAI.cpp", "tests/regression/test_speed_invariance.cpp"], "verifyCommand": "grep -rn \"FPS_ANIMATION_FACTOR\\|REFERENCE_FPS\" src/source; ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure", "acceptanceCriteria": ["grep FPS_ANIMATION_FACTOR = 0", "grep REFERENCE_FPS = 0", "suite regresion E1-E6 verde", "ctest verde"], "userGate": true, "tags": ["user-gate"]}
```

---

## Self-review (cobertura del spec)

- **§5 etapas 0–8** → Tasks 1–10 (0→T1, 1→T2/T3, 2→T4, 3→T5, 4→T6, 5→T7, 6→T8, 7→T9, 8→T10). ✔
- **§8 modelo de test (extraer a puro + doctest)** → cada task extrae función pura + test doctest; patrón `mu_add_test`. ✔
- **§3 medición E1–E6** → Task 1 instrumenta; Tasks 3–10 verifican el E# correspondiente; Task 10 automatiza la suite. ✔
- **§6 worktree/secuencial/tags** → comandos por task con commit + tag `temporal/stage-NN`. ✔
- **§10 game loop** → Task 3 inserta acumulador + FixedUpdate + alpha. ✔
- **§14 criterios de éxito** → mapeados a los Verify de cada task; cierre por grep en Task 10. ✔
- Placeholders: Tasks 1–3 con código completo; Tasks 4–10 con implementación deliberadamente derivada del deep-dive (metodología elegida por el usuario), pero con deliverable, sitios objetivo y criterio E# concretos. ✔
- Consistencia de tipos: `SimulationClock`/`StepResult`/`ClockConfig`, `MovementProbe`, `IntegratePosition`, `DecayLight` referidos consistentemente. ✔
