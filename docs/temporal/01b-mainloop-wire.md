# Etapa 1b — Cablear SimulationClock en el MainLoop (factor ≡ 1.0)

> **Estado:** Deep-dive de investigación. **GATE A** — requiere aprobación del usuario antes de tocar código de engine.
> **Precede a:** Red→Green→Verify de la Etapa 1b. **Depende de:** Stage 0 (MovementProbe ✅), Stage 1 (SimulationClock ✅).

---

## Qué cambia exactamente

Hoy la **simulación corre dentro del frame de render**: `MainLoop` llama `RenderScene(hDC)` solo cuando `CheckRenderNextFrame()` (cap de FPS) lo permite, y `RenderScene` ejecuta *primero* la sim (`UpdateSceneState()`) y *después* el dibujo (`MainScene(hDC)`). Por eso, subir el FPS = más llamadas a sim/seg = speedhack; el único parche es `FPS_ANIMATION_FACTOR = clamp(REFERENCE_FPS/FPS, 0, 1)` multiplicado en ~cientos de call-sites.

Stage 1b **separa el avance de mundo del dibujo** y lo gobierna con `SimulationClock`:
la sim corre N pasos fijos de **40 ms (25 tps)** por iteración del loop según el tiempo real transcurrido, y el render dibuja a su propio ritmo (cap de `g_frameTiming` sin cambios). Como cada `FixedUpdate` equivale exactamente a **un tick de 25 FPS**, se fuerza **`FPS_ANIMATION_FACTOR ≡ 1.0`**: los cientos de call-sites `valor * FPS_ANIMATION_FACTOR` siguen multiplicando, pero ahora por 1.0, avanzando "una unidad por tick" — idéntico al baseline de 25 FPS, a cualquier FPS de render. No se migra ningún subsistema todavía (eso es Stage 2+); esta etapa solo cambia **quién y cuántas veces** llama a la sim.

**La costura ya existe y es limpia** (hallazgo clave de la investigación): dentro de `RenderScene`, `UpdateSceneState()` (sim) y `MainScene(hDC)` (draw) son secuenciales, no están interleaved. No hay que desenredar dibujo de lógica frame por frame; hay que **cortar en ese límite** y mover la mitad sim al loop fijo.

---

## Sitios afectados

| Archivo:línea | Símbolo | Rol hoy | Cambio en 1b | Cat |
|---|---|---|---|---|
| `App/Platform/Windows/Winmain.cpp:1106-1142` | `MainLoop` bloque render | `if(CheckRenderNextFrame()) RenderScene(hDC)` | Insertar acumulador: `for(steps) FixedUpdateScene(); ... RenderScene(hDC, alpha)` | — |
| `Scenes/SceneManager.cpp:1044-1086` | `RenderScene(HDC)` | sim **+** draw soldados | Partir en `FixedUpdateScene()` (sim) y `RenderScene(HDC, float alpha)` (draw) | — |
| `Scenes/SceneManager.cpp:281-289` | `UpdateSceneState()` | sim entry (input+mundo) | Pasa a llamarse desde `FixedUpdateScene()` | — |
| `Scenes/SceneCore.h:27` / `SceneManager.h:110` | `RenderScene` decl | `void RenderScene(HDC)` | `void RenderScene(HDC, float alpha)` (alpha sin uso hasta Stage 2) | — |
| `Engine/AI/ZzzAI.cpp:711-747` | `CalcFPS()` | mide FPS display, setea factor, `WorldTime`, `CalcSkillDelay` | Partir: FPS/`WorldTime` display → render; `FPS_ANIMATION_FACTOR=1.0` + `CalcSkillDelay(FIXED_DT)` → fixed | E |
| `Engine/AI/ZzzAI.cpp:702` | `FPS_ANIMATION_FACTOR` (def) | `clamp(REFERENCE_FPS/FPS,0,1)` por frame | Forzado a `1.0f` durante toda la etapa | E |
| `Scenes/MainScene.cpp:311-343` | `MoveMainScene()` | `UpdateUIAndInput()` + `UpdateGameEntities()` | Sin cambios internos; corre dentro del tick fijo (ver Riesgo input) | A |

> No se tocan los ~cientos de call-sites de `FPS_ANIMATION_FACTOR` (Camera, World/GameMaps, ZzzEffect…): quedan multiplicando por 1.0. Su migración real es Stage 2–8.

---

## Comportamiento actual → objetivo

**Hoy** (`Winmain.cpp:1106`):
```
while(!Destroy){
  poll/input; DrainPackets; Reconnect::Begin; FrameTimerScheduler::Tick;
  if (CheckRenderNextFrame() && active)   // gate por cap de FPS
      RenderScene(hDC);                    // CalcFPS → UpdateSceneState (SIM) → MainScene (DRAW)
  else
      WaitForNextActivity();               // pacing
}
```
Sim/seg = render/seg → escala con FPS. Factor compensa solo cat A; clamp `[0,1]` causa E2 (a 10 FPS, factor pin 1.0 → sim al ~40%).

**Objetivo Stage 1b:**
```
while(!Destroy){
  poll/input; DrainPackets; Reconnect::Begin; FrameTimerScheduler::Tick;

  nowMs   = g_pTimer->GetTimeElapsed();
  frameMs = nowMs - prevMs; prevMs = nowMs;
  step    = s_simClock.Advance(frameMs);      // SimulationClock (Stage 1, ya testeado)
  for (int i=0;i<step.steps;++i)
      FixedUpdateScene();                      // SIM a 25 tps fijo (factor≡1.0)

  if (CheckRenderNextFrame() && active)
      RenderScene(hDC, step.alpha);            // solo DRAW; alpha sin uso aún
  else
      WaitForNextActivity();
}
```
Sim/seg = 25 constante (lo gobierna el reloj de pared, no el FPS). Render/seg = lo que permita `g_frameTiming`. E1 plano, E2 sin caída a bajo FPS, spiral cortada por `maxSteps`.

---

## Lógica a extraer / mover (sin módulo nuevo: el puro ya existe)

`SimulationClock` (Stage 1) ya es la pieza pura testeada. Stage 1b es **integración**, no extracción nueva. Reparto de `CalcFPS()` y `RenderScene()`:

```
// FixedUpdateScene()  — corre N veces/frame, dt = 40ms
//   FPS_ANIMATION_FACTOR = 1.0f;          // invariante de balance
//   UpdateSceneState();                   // sim de mundo + input (ver Riesgo)
//   gSkillManager.CalcSkillDelay(40);     // antes en CalcFPS con differenceMs real

// RenderScene(HDC hDC, float alpha)  — corre 0..1 vez/frame
//   <medición de FPS display + WorldTime para HUD/g_Luminosity>
//   ReconnectManager::Update();
//   g_frameTiming.MarkFrameRendered();
//   MainScene(hDC);                        // draw + SwapBuffers
```

Firma nueva: `RenderScene(HDC, float alpha)` (`alpha` documentado "reservado Stage 2"). Acumulador: `static Core::Time::SimulationClock s_simClock;` en `MainLoop`.

---

## Plan de test

El acumulador ya está cubierto (Stage 1, 6/6: invariancia 8ms↔40ms = 25 steps, spiral cortada, clamps). Stage 1b agrega **un test de integración pura** + **verificación empírica E1/E2**.

- **Test puro nuevo** `tests/time/test_fixedstep_drive.cpp`: simular el bucle `Advance`+contador-de-FixedUpdate sobre una traza de frames variables (jitter 4–60 ms durante 10 s) y verificar:
  - total de `FixedUpdate` ≈ `wallMs/40` (±1 step) → invariancia frame-rate del *driver*.
  - tras un stall de 1000 ms, no hay "ráfaga" > `maxSteps` (deuda descartada).
- **E1 empírico** (necesita cliente + baseline Task 1): `hero_units_per_sec` a 30/60/144 FPS coincide con baseline ±2%.
- **E2 empírico**: con cap a 10 FPS la sim NO cae al ~40% (clamp dejó de gobernar).

> El test puro NO necesita engine: reusa `SimulationClock` y un lambda contador. Cae en `tests/time/` con `mu_add_test`.

---

## Riesgo + mitigación

1. **WorldTime (riesgo alto).** `WorldTime = g_WorldTime->GetTimeElapsed()` (reloj de pared) lo usan tanto render (`g_Luminosity = sin(WorldTime…)`, `SceneManager.cpp:1058`) como sim/animaciones. Si la sim corre en pasos fijos pero `WorldTime` sigue siendo wall-clock, las animaciones basadas en `WorldTime` no cambian de ritmo (bien para 1b), pero las basadas en *conteo de ticks* sí. **Mitigación 1b:** mantener `WorldTime` como wall-clock (medido en el render), NO cambiar su semántica todavía; solo forzar `FPS_ANIMATION_FACTOR=1.0`. Documentar que la migración de animaciones es Stage 4.
2. **Tasa de muestreo de input (riesgo medio).** `UpdateUIAndInput()` vive dentro de `MoveMainScene()` (sim). Correrlo N veces/frame podría doble-procesar clicks cuando N>1 (solo a <25 FPS). **Mitigación recomendada:** en 1b, dejar input dentro del tick fijo (N=0/1 a FPS normal ≥25; el doble-proceso solo ocurre en stalls, donde ya hay degradación aceptable). *Alternativa* (si se prefiere): extraer `UpdateUIAndInput()` a una llamada única por frame de render. **← decisión para el Gate.**
3. **`frameMs` inicial / primer frame.** `prevMs` debe inicializarse con `g_pTimer->GetTimeElapsed()` antes del primer `Advance`, si no el primer `frameMs` es enorme (lo absorbe el clamp `maxFrameMs=250`, pero mejor evitarlo).
4. **`CalcSkillDelay`.** Hoy recibe `differenceMs` real; pasa a recibir `FIXED_DT` (40). Verificar que ningún delay dependa de FPS variable (debería ser al revés: 40 fijo lo hace correcto).
5. **`ReconnectManager::Update()` / `MarkFrameRendered()`.** Son de cadencia de render → quedan en `RenderScene`. Confirmar que ningún estado de reconnect dependa de correr cada iteración de sim.

---

## Criterio de éxito

- **E1** plano ±2% a 30/60/144 FPS vs baseline (`docs/temporal/baseline/`).
- **E2**: velocidad sostenida con cap a 10 FPS (sin caída al ~40%).
- Sin spiral: stall de 1 s no acelera después (test `droppedDebt` + observación runtime).
- `cmake --build` + `ctest` verdes (incluido el nuevo `test_fixedstep_drive`).
- `grep` runtime/assert: `FPS_ANIMATION_FACTOR == 1.0f` siempre durante la etapa.

---

## Rollback

Cambio acotado a 4 archivos. Rollback = `git revert` del commit de la etapa o `git reset` al tag `temporal/stage-01`. La firma `RenderScene(HDC, float)` es el único cambio de interfaz; revertir restaura `RenderScene(HDC)`. Ningún cambio de datos/persistencia. Banderín de seguridad: si E1/E2 no quedan planos, NO se taggea `temporal/stage-01b` y se revierte.

---

## Refinamiento as-built (implementado)

Tras leer el dispatch de escenas, el diseño se **concentró en `RenderScene` y se acotó a `MAIN_SCENE`**, más simple y de menor riesgo que la propuesta original (que tocaba `Winmain.cpp` y cambiaba la firma de `RenderScene`):

- **`Winmain.cpp` NO se toca.** El acumulador vive en `RenderScene` (que ya corre 0..1 vez por frame, gated por `CheckRenderNextFrame`). `Advance()` se llama una vez por **frame de render** con el delta `WorldTime` desde el render anterior → la deuda sub-tick queda en el acumulador. Sin pérdida de steps entre frames.
- **Fixed-step solo en `MAIN_SCENE`.** Las escenas de menú (login/character/loading) siguen corriendo una vez por render con su compensación `FPS_ANIMATION_FACTOR` clampeada intacta. Por eso `CalcFPS` fuerza `factor=1.0` **solo** si `SceneFlag==MAIN_SCENE`; el resto mantiene `clamp(REFERENCE_FPS/FPS)`. Esto evita un speedhack de menú y hace que el smoke-test de login/character sea representativo.
- **Costura real:** `UpdateGameEntities()` se extrajo de `MoveMainScene()` a una función pública nueva **`MainSceneFixedUpdate()`** (mundo). `MoveMainScene()` queda con init/gating/UI/**input** (corre una vez por frame vía `UpdateSceneState`, satisfaciendo "input una vez por frame de render" sin refactor cross-scene). `RenderScene` corre `for(steps) MainSceneFixedUpdate()`.
- **Firma intacta:** `RenderScene(HDC)` no cambia; `alpha` se calcula y queda disponible (`simAlpha`) para que Stage 2 lo consuma. El CSV ahora registra `steps`/`alpha` reales.

Archivos realmente tocados: `Engine/AI/ZzzAI.cpp` (CalcFPS), `Scenes/MainScene.cpp` + `.h` (split), `Scenes/SceneManager.cpp` (driver en RenderScene). `Winmain.cpp`, `SceneCore.h` → **sin cambios**.

> **Verificación:** test puro `tests/time/test_fixedstep_drive.cpp` (driver con jitter, invariancia de ticks). Empírico E1/E2 **diferido** — el cliente no conecta al server mientras está en curso el trabajo de servidor autoritativo (ver memoria `authoritative-server-empirical-deferred`). Smoke-test posible: login/character (corren sin server).

## Decisiones tomadas (Gate A — aprobado)

1. **Input rate:** **una vez por frame de render.** `UpdateUIAndInput()` se **extrae** de `MoveMainScene()` y se llama una sola vez en el path de render (no N veces en el tick fijo). `UpdateGameEntities()` (mundo) queda en el tick fijo. Implica refactor de `MoveMainScene` (separar input de mundo) — parte de la cirugía 1b.
2. **`WorldTime`:** se mantiene **wall-clock** en 1b (medido en el render). Su tick-ificación se difiere a Stage 4.
3. **Orden:** **baseline primero.** Completar CSV-wiring + captura de baseline (Task 1) con cliente corriendo → recién después cablear 1b y comparar E1/E2.
