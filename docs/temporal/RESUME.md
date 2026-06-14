# Desacople temporal — Estado y guía de reanudación

> **Estado (junio 2026):** Stage 1b **verificado empíricamente** (tag `temporal/stage-01b`). El cliente temporal **conecta** al server local (vía cmd/bat, sin fix de código — ver `authoritative-server-empirical-deferred` en memoria). Siguiente: Stage 2.

## TL;DR

El cambio central — **desacoplar la simulación del FPS de render (fix del speedhack)** — está **implementado, commiteado y verificado en runtime**: velocidad del Hero plana a 30/60/144 FPS (288/283/301 u/s, ver `01b-results.md`). Todo el trabajo vive aislado en una worktree/branch propia; **no afecta `main` ni el trabajo de servidor**. El cliente temporal conecta al OpenMU local lanzándolo por `run-temporal-client.bat`.

## Dónde vive todo

- **Worktree:** `I:\MuOnline\MuMain-temporal` (separada; podés seguir trabajando en `I:\MuOnline` sin interferencia).
- **Branch:** `temporal/integration` (sale del primer commit del usuario `7a4859b7`, solo docs — sin arrastrar cambios de trabajo).
- **Aislamiento:** los cambios de runtime de Stage 1b están **commiteados en esta branch**, no en `main`. El cliente de tu árbol normal no cambia.

## Commits (de más nuevo a más viejo)

| Commit | Qué |
|---|---|
| `5c9453d6` | **Stage 1b** — fixed-step del mundo en MAIN_SCENE (code-complete) |
| `85927dbf` | Stage 0 — CSV logger (`TemporalCsvLogger`) + cableado en `RenderScene` |
| `fceecacd` | Stage 1 — `SimulationClock` puro (tag `temporal/stage-01`) |
| `5602a54a` | Stage 0 — `MovementProbe` puro + doctest |
| `dbc6815d` | docs — spec + plan + tasks |

## Estado por etapa

| Stage | Código | Tests puros | Empírico (runtime) |
|---|---|---|---|
| 0 — MovementProbe + CSV | ✅ | ✅ 3/3 | ⏸ baseline diferido |
| 1 — SimulationClock | ✅ | ✅ 6/6 | n/a (puro) |
| 1b — fixed-step MAIN_SCENE | ✅ | ✅ 2/2 | ✅ E1 verificado (tag stage-01b); E2 pendiente |
| 2 — movimiento local (interp) | 📄 deep-dive listo (Gate A sin aprobar) | — | ⏸ |
| 3–8 | ⬜ pendiente | — | ⏸ |

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

## Cómo retomar (cuando el cliente vuelva a conectar)

1. **Capturar baseline** (Task 1, diferido). Activar el flag y entrar al mundo:
   `MU_TEMPORAL_CSV=C:\...\docs\temporal\baseline\fps60.csv` → caminar el Hero. Repetir a 30/60/144 FPS. Detalle en `docs/temporal/baseline/README.md`.
   - Decisión previa: **auto-walk de debug** (Hero camina trayecto fijo en timer) para capturas repetibles automáticas. Construirlo al retomar.
2. **Verificar Stage 1b empírico:** E1 (`hero_units_per_sec` plano ±2% a 30/60/144) y E2 (sin caída a 10 FPS). Si pasa → taggear `temporal/stage-01b`.
   - *Opción A/B útil:* meter un toggle runtime del fixed-step para comparar on/off en vivo (idea del usuario "probar 1×1").
3. **Continuar Stage 2** — deep-dive ya escrito en `docs/temporal/02-movimiento-local.md` (Gate A pendiente de tu OK). Aporta interpolación de render (suavidad), porque post-1b el Hero salta a 25 Hz.
4. **Stages 3–8** — seguir el plan `docs/superpowers/plans/2026-06-13-temporal-decoupling.md`, etapa por etapa: deep-dive doc → Gate A → función pura + doctest → integración → verificar E# → commit + tag.

## Decisiones lockeadas (no re-litigar)

- Granularidad = por etapa (0–8). Investigación = deep-dive doc por etapa **antes** de tocar código (Gate A).
- Test = extraer a **función pura + doctest** (la extracción ES la mejora). `mu_add_test` por módulo.
- Loop = self-paced con gate por iteración. Secuencial en una sola branch (no worktrees paralelos).
- 1b: **input una vez por frame de render**; `WorldTime` queda wall-clock (tick-ificación → Stage 4).
- Estrategia bajo bloqueo de server: código + tests puros, empírico diferido (aceptado el riesgo de deuda de verificación).

## Constraint vigente

Servidor autoritativo en curso → packets cambian → cliente no conecta → no hay `MAIN_SCENE` → toda verificación empírica diferida. Memoria: `authoritative-server-empirical-deferred`.

## Índice de docs

- `docs/temporal/RESUME.md` — este archivo (punto de entrada al retomar).
- `docs/temporal/01b-mainloop-wire.md` — deep-dive Stage 1b (as-built).
- `docs/temporal/02-movimiento-local.md` — deep-dive Stage 2 (Gate A pendiente).
- `docs/temporal/baseline/README.md` — procedimiento de captura de baseline.
- `docs/superpowers/specs/2026-06-13-temporal-decoupling-design.md` — spec.
- `docs/superpowers/plans/2026-06-13-temporal-decoupling.md` — plan 10 tareas.
- `docs/build-toolchain-vs-insiders.md` — toolchain VS Insiders.
