# Desacople temporal — Estado y guía de reanudación

> **Estado (junio 2026):** Stages 1b, 2, 3, 4a y 4b **verificados empíricamente con logs** (tags `-01b`, `-02`, `-03`, `-04a`, `-04b`). **Stage 4 COMPLETO.** Bonus: crash de shutdown (exit 139) **localizado con cdb y arreglado** (`FrameTimerScheduler`). El cliente conecta, entra al mundo y **cierra limpio**. Siguiente: **Stage 5** (cámara cinemática, user-gate). **Antes de tocar animación/física/efectos/cámara: leer [`KNOWN-ISSUES.md`](KNOWN-ISSUES.md)** (riesgos del factor=1.0 + regla: no meter estado de render en `OBJECT`).
>
> **Lanzar cliente (recetas que funcionan):** `Main.exe` directo desde Bash con `export MSYS2_ARG_CONV_EXCL="*"` (evita el mangle de `/u.../p...`) + `cd` al dir `Debug` + path absoluto al exe. Para capturar CSV: `export MU_TEMPORAL_CSV="<path absoluto>"` antes. El `run-temporal-client.bat` falla por `cmd.exe //c` con paths relativos (NoDefaultCurrentDirectoryInExePath). **No lanzar sin OK del usuario.**

## TL;DR

El cambio central — **desacoplar la simulación del FPS de render (fix del speedhack)** — está **implementado, commiteado y verificado en runtime**: velocidad del Hero plana a 30/60/144 FPS (288/283/301 u/s, ver `01b-results.md`). Todo el trabajo vive aislado en una worktree/branch propia; **no afecta `main` ni el trabajo de servidor**. El cliente temporal conecta al OpenMU local lanzándolo por `run-temporal-client.bat`.

## Dónde vive todo

- **Worktree:** `I:\MuOnline\MuMain-temporal` (separada; podés seguir trabajando en `I:\MuOnline` sin interferencia).
- **Branch:** `temporal/integration` (sale del primer commit del usuario `7a4859b7`, solo docs — sin arrastrar cambios de trabajo).
- **Aislamiento:** los cambios de runtime de Stage 1b están **commiteados en esta branch**, no en `main`. El cliente de tu árbol normal no cambia.

## Commits (de más nuevo a más viejo)

| Commit | Qué |
|---|---|
| `0b1de350` | **Stage 4b** — pose del cuerpo suave entre ticks (tag `temporal/stage-04b`) |
| `8465d990` | **fix** — leak de `FrameTimerScheduler` → cierre limpio (mata exit-139) |
| `36db325b` | **Stage 4a** — avance de animación render-path independiente del FPS (tag `temporal/stage-04a`) |
| `deaf8a18` | **Stage 3** — interp de render de entidades remotas + toggle `$interp` (tag `temporal/stage-03`) |
| `22bf9c6a` | **Stage 2** — interp de render del Hero (tag `temporal/stage-02`) |
| `5c9453d6` | **Stage 1b** — fixed-step del mundo en MAIN_SCENE (tag `temporal/stage-01b`) |
| `85927dbf` | Stage 0 — CSV logger (`TemporalCsvLogger`) + cableado en `RenderScene` |
| `fceecacd` | Stage 1 — `SimulationClock` puro (tag `temporal/stage-01`) |
| `5602a54a` | Stage 0 — `MovementProbe` puro + doctest |
| `dbc6815d` | docs — spec + plan + tasks |

## Estado por etapa

| Stage | Código | Tests puros | Empírico (runtime) |
|---|---|---|---|
| 0 — MovementProbe + CSV | ✅ | ✅ 3/3 | ⏸ baseline diferido |
| 1 — SimulationClock | ✅ | ✅ 6/6 | n/a (puro) |
| 1b — fixed-step MAIN_SCENE | ✅ | ✅ 2/2 | ✅ E1 (288/283/301 u/s @30/60/144) + E2 (10fps OK) |
| 2 — movimiento local (interp Hero) | ✅ | ✅ 4/4 | ✅ visual suave + regresión 1.8% (tag stage-02) |
| 3 — entidades remotas (interp + `$interp`) | ✅ | ✅ reusa 4/4 | ✅ logs: render-mov 64.8% vs raw 24.3% @144; toggle off→render==raw 100% (tag stage-03) |
| 4a — animación render-path (partes) | ✅ | ✅ 5/5 | ✅ logs: tasa avance plana 25.0/s @30/60/144 (vs OLD ∝FPS) (tag stage-04a) |
| 4b — pose del cuerpo suave (P2) | ✅ | ✅ 7/7 | ✅ logs: pose render-chg 97.3% vs raw 41.9% @60; render≠raw 95.4% (tag stage-04b) |
| 5–8 | ⬜ pendiente | — | — |

**Tests puros totales: 11 casos / 23 assertions, todo verde.** Build `Main.exe` exit 0.

## Qué hace Stage 1b (el fix principal)

La sim del mundo (`MAIN_SCENE`) corre a **25 tps fijos** dentro de `RenderScene`, manejada por `SimulationClock` según tiempo real (delta de `WorldTime`). Subir/bajar FPS ya no acelera/frena el juego. `FPS_ANIMATION_FACTOR` se fuerza a `1.0` **solo en MAIN_SCENE** (el menú mantiene su compensación). Diseño as-built: concentrado en 4 archivos, **sin tocar `Winmain` ni firmas**.

- `Engine/AI/ZzzAI.cpp` — `CalcFPS`: factor=1.0 si MAIN_SCENE, sino clamp.
- `Scenes/MainScene.cpp` + `.h` — `UpdateGameEntities` extraído a `MainSceneFixedUpdate()`.
- `Scenes/SceneManager.cpp` — driver fixed-step en `RenderScene`; puebla `steps`/`alpha` del CSV.

## Build & test (worktree `MuMain-temporal`)

Toolchain VS 2026 Insiders vía wrapper `C:\Users\ipera\AppData\Local\Temp\mu_build.bat` (regenerable; ver `docs/build-toolchain-vs-insiders.md`).

```bash
# build cliente + tests
cmd.exe /c "C:\Users\ipera\AppData\Local\Temp\mu_build.bat cmake --build --preset windows-x86-debug"

# tests puros (correr los .exe directo; doctest registra por nombre de TEST_CASE)
./out/build/windows-x86/tests/diagnostics/Debug/test_movement_probe.exe
./out/build/windows-x86/tests/time/Debug/test_simulation_clock.exe
./out/build/windows-x86/tests/time/Debug/test_fixedstep_drive.exe
```

## Cómo retomar (Stage 5 en adelante)

1. **Siguiente = Stage 5 (cámara cinemática) — USER-GATE.** Portar travel/zoom de la cámara cinemática (sitios cat E en `CameraMove.cpp` 388-389, 402-407, 428-429) a dt real, para que cutscenes duren igual a todo FPS. Es render-path con factor=1.0 → acoplado (ver `KNOWN-ISSUES.md` #3). Flujo: deep-dive `05-camara-cinematica.md` → Gate A → función pura + doctest (`tests/camera`) → integración → log → commit + tag. Plan: `docs/superpowers/plans/2026-06-13-temporal-decoupling.md`.
2. **Flujo por etapa:** deep-dive doc → Gate A (OK del usuario) → función pura + doctest → integración → verificar empírico con **logs/CSV** → commit + tag `temporal/stage-0N`.
3. **Verificación = logs, no inspección visual.** El usuario entra in-game y hace las acciones pedidas; la prueba debe dejar CSV analizable (extender `TemporalCsvLogger` + analizador en `baseline/`). **No lanzar el cliente sin OK explícito.**
4. **Toggles en chat para A/B en vivo:** `$vsync off`, `$fps <n>` (`-1`=ilimitado), `$interp on/off`.

## Decisiones lockeadas (no re-litigar)

- Granularidad = por etapa (0–8). Investigación = deep-dive doc por etapa **antes** de tocar código (Gate A).
- Test = extraer a **función pura + doctest** (la extracción ES la mejora). `mu_add_test` por módulo.
- Loop = self-paced con gate por iteración. Secuencial en una sola branch (no worktrees paralelos).
- 1b: **input una vez por frame de render**; `WorldTime` queda wall-clock (tick-ificación → Stage 4).
- Estrategia bajo bloqueo de server: código + tests puros, empírico diferido (aceptado el riesgo de deuda de verificación).

## Constraint vigente

- **No lanzar el cliente sin OK del usuario** (él coordina el estado del server). Memoria: `no-launch-client-without-confirmation`.
- **Toda verificación se apoya en logs analizables** (CSV crudo vs render), no solo inspección visual.
- El cliente temporal **conecta** al server local; la verificación empírica ya **no** está diferida.
- **Issues:** ver [`KNOWN-ISSUES.md`](KNOWN-ISSUES.md). Resueltos: #1 crash shutdown (exit 139, `FrameTimerScheduler`), #2 pose choppy (4b). Abierto #3: sitios render-path con factor=1.0 sin compensar (efectos/física → Stage 6; cámara → Stage 5). No bloquean.
- **Debugger:** WinDbg instalado; cdb x86 en `WindowsApps/cdbX86.exe`. Receta de captura de stack en `KNOWN-ISSUES.md` (`cdb-crash.txt`).

## Índice de docs

- `docs/temporal/RESUME.md` — este archivo (punto de entrada al retomar).
- `docs/temporal/KNOWN-ISSUES.md` — **issues abiertos + audit de `FPS_ANIMATION_FACTOR` (riesgos derivados).** Leer antes de tocar anim/física/efectos/cámara.
- `docs/temporal/03-entidades-remotas.md` — deep-dive Stage 3 (as-built + resultados).
- `docs/temporal/04-animaciones.md` — deep-dive Stage 4 (as-built 4a + plan 4b).
- `docs/temporal/01b-mainloop-wire.md` — deep-dive Stage 1b (as-built).
- `docs/temporal/02-movimiento-local.md` — deep-dive Stage 2 (Gate A pendiente).
- `docs/temporal/baseline/README.md` — procedimiento de captura de baseline.
- `docs/superpowers/specs/2026-06-13-temporal-decoupling-design.md` — spec.
- `docs/superpowers/plans/2026-06-13-temporal-decoupling.md` — plan 10 tareas.
- `docs/build-toolchain-vs-insiders.md` — toolchain VS Insiders.
