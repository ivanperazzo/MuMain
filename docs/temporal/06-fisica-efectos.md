# Etapa 6 — Física de efectos / partículas (cat D/C/B✗ → dt real)  ·  USER-GATE

> Deep-dive. Verificación empírica E3 (alcance proyectiles), E4 (densidad partículas/colas),
> E6 (fades), planos a 30/60/144. Gate A: alcance/fases.

## Causa raíz (la misma de siempre)

Todos estos sitios hacen un incremento por **frame de render** multiplicado por
`FPS_ANIMATION_FACTOR`:
```cpp
o->Position[2] += 100.f * FPS_ANIMATION_FACTOR;   // mov / scatter
o->LifeTime    -= 60   * FPS_ANIMATION_FACTOR;    // decay lineal
o->Direction[2]*= pow(0.8f, FPS_ANIMATION_FACTOR);// decay exponencial
o->Timer       += 0.2f * FPS_ANIMATION_FACTOR;    // timer
```
Pre-1b `factor = REFERENCE_FPS/FPS` los hacía FPS-independientes (incremento chico a alto
FPS). **1b fijó `factor=1.0` en MAIN_SCENE** ⇒ incremento completo cada frame ⇒ a 144 FPS
los efectos corren/decaen/expiran ~5.8× más rápido y las partículas se dispersan más.
(En login/menú el factor sigue el clamp ⇒ esos efectos siguen OK; el bug es solo MAIN_SCENE.)

## Sitios por categoría (líneas actuales)

| Cat | Patrón | Archivos (ejemplos) | Fix |
|---|---|---|---|
| **D** mov/integración lineal | `Pos += v*factor`, `VectorAddScaled(Pos,d,Pos,factor)`, `vel += g*factor` | `ZzzEffect.cpp` (misiles 579-864), `ZzzEffectParticle.cpp` (128-553+), `ZzzEffectPoint.cpp` (146), `ZzzEffectJoint.cpp` (673,1173,1456,1798,2354…), `CSPetSystem.cpp` (513,528,572), `GOBoid.cpp` (641,1012-1014,1099), `PhysicsManager.cpp` (forces 76-104, cloth) | `* dt` (dt=frameMs/40) |
| **D** decay lineal / lifetime | `LifeTime -= N*factor`, `Scale -= N*factor`, `Gravity -= N*factor` | `ZzzEffectPoint.cpp` (136,148,153), `ZzzEffect.cpp` (621), `CSPetSystem.cpp` (530), `GOBoid.cpp` (1000) | `* dt` |
| **B✗** decay exponencial | `x *= pow(k, factor)` | `GOBoid.cpp` (942,947), maps `GM_*` (luz) | `pow(k, dt)` |
| **C** conteos enteros / thresholds | `MaxTails = MaxTails / factor`, `WeaponLevel == (BYTE)(19/factor)` | `ZzzEffectJoint.cpp` (2709), `GOBoid.cpp` (763) | rediseñar a tiempo/ticks (NO dividir por dt → overflow) |
| **C** timers/counters | `Timer += N*factor`, `AttackTime += factor`, `MultiUse += factor` | `GOBoid.cpp` (928), `CSPetSystem.cpp` (530,586), `ZzzEffectJoint.cpp` (1251) | `+= N*dt` (tiempo real) |

Conteo aprox: ~100 sitios. La mayoría (cat D lineal + B✗ + timers) son un **swap mecánico**;
solo cat-C-conteos y la cloth de `PhysicsManager` necesitan cuidado extra.

## Fix unificado

`dt` = duración real del frame normalizada al tick de referencia = `frameMs / 40`
(= lo que `REFERENCE_FPS/FPS` aproximaba, pero con tiempo real). Ya está disponible:
`Render::Interpolation::FrameMs()` (Stage 4a).

Helper **scene-aware** (glue) + math **pura** (testeable):
```cpp
// puro (tests/effects): EffectTiming
float LinearStep(double frameMs);          // = clamp(frameMs)/40   (para * dt)
float DecayPow(float k, double frameMs);   // = pow(k, clamp(frameMs)/40)
// glue (lee SceneFlag/FrameMs/FPS_ANIMATION_FACTOR):
float EffectStep();   // MAIN_SCENE -> LinearStep(FrameMs()); else FPS_ANIMATION_FACTOR
```
⚠️ **Scene-aware obligatorio:** fuera de MAIN_SCENE hay que mantener `FPS_ANIMATION_FACTOR`
(login/menú siguen con el clamp). Por eso `EffectStep()` cae al factor original si no es
MAIN_SCENE / no hay frameMs. Reemplazo: `* FPS_ANIMATION_FACTOR` → `* EffectStep()`,
`pow(k, FPS_ANIMATION_FACTOR)` → `pow(k, EffectStep())`.

**No re-tuning para cat D lineal / B✗ / timers:** el swap reproduce el feel pre-1b al
reference (25 tps). Solo cat-C-conteos y cloth pueden requerir ajuste.

## Fases propuestas (Gate A)

- **6a — decays + lifetimes + timers (E6 fades):** swap mecánico de cat B✗, decay lineal,
  lifetimes y timers. Bajo riesgo, alto valor visible (fades/efectos no se aceleran). Helper
  puro + doctest. Log: vida/fade de un efecto plano a 30/60/144.
- **6b — integración de movimiento (cat D lineal, E3/E4):** misiles/partículas/colas/pet/boid
  `Pos += v*factor` → `*dt`. Riesgo medio (alcance/arco, scatter aleatorio). Log: alcance de
  proyectil + densidad de partículas plana.
- **6c — conteos cat C + cloth `PhysicsManager`:** `MaxTails`/thresholds a tiempo; cloth con
  sub-stepping fijo si hace falta (lo más delicado). Riesgo alto.

## Verificación (logs)

Extender CSV o un probe dedicado: capturar a 30/60/144 (a) `LifeTime`/fade de un efecto fijo
(E6), (b) alcance final de un proyectil lanzado (E3), (c) nº de partículas/segmentos de cola
vivos (E4). Objetivo: planos entre FPS. El usuario dispara el efecto/skill in-game; el log
registra. (Como Stages 3/4: nada de inspección visual sola.)

## Riesgo / rollback

- 6a/6b: swap acotado, `EffectStep()` cae al comportamiento original fuera de MAIN_SCENE.
- 6c: cloth/colas pueden volverse inestables con dt variable → sub-stepping.
- Rollback por fase: `git revert` del commit.

---

## As-built 6a (HECHO + verificado)

**Módulo:** `Render/EffectTiming.h` + `EffectTiming.cpp` (puro, testeable) +
`EffectTimingGlue.cpp` (scene-aware, lee `SceneFlag`/`FrameMs()`/`FPS_ANIMATION_FACTOR`).
- `LinearStep(frameMs)` = `clamp(frameMs,250)/40`, `<=0 → 1.0`.
- `DecayPow(base,frameMs)` = `pow(base, LinearStep)`.
- `EffectStep()` = MAIN_SCENE con frameMs>0 → `LinearStep(FrameMs())`; si no → `FPS_ANIMATION_FACTOR`.
- `EffectDecayExp(base)` = MAIN_SCENE → `DecayPow`; si no → `pow(base, FPS_ANIMATION_FACTOR)`.

**Sitios swappeados (decay lineal / lifetime / timer / decay exp en MAIN_SCENE):**
| Archivo | Línea | Antes | Después |
|---|---|---|---|
| `Render/Effects/ZzzEffectPoint.cpp` | LifeTime | `-= FACTOR` | `-= EffectStep()` |
| `Render/Effects/ZzzEffectPoint.cpp` | Gravity (decay) | `-= 0.3f*FACTOR` | `-= 0.3f*EffectStep()` |
| `Render/Effects/ZzzEffectPoint.cpp` | Scale | `-= 5.f*FACTOR` | `-= 5.f*EffectStep()` |
| `Engine/AI/GOBoid.cpp` | MoveBat Timer | `+= 0.2f*FACTOR` | `+= 0.2f*EffectStep()` |
| `Engine/AI/GOBoid.cpp` | Butterfly damping ×2 | `*= pow(0.8f,FACTOR)` | `*= EffectDecayExp(0.8f)` |
| `Engine/AI/GOBoid.cpp` | BOID_UP Velocity | `-= 0.005f*FACTOR` | `-= 0.005f*EffectStep()` |
| `GameLogic/Pets/CSPetSystem.cpp` | pet LifeTime | `+= FACTOR` | `+= EffectStep()` |
| `GameLogic/Pets/CSPetSystem.cpp` | AttackTime | `+= FACTOR` | `+= EffectStep()` |

**Desviaciones del plan (sitios NO tocados — el plan los listaba pero son one-shot/conteo,
no decays per-frame; en MAIN_SCENE con factor=1.0 YA son correctos y el swap los rompería):**
- `Render/Effects/ZzzEffect.cpp:621` `LifeTime -= 60*FACTOR` — es **init one-shot** en
  `CreateEffect` (asigna `LifeTime=1000` absoluto justo antes). Con factor=1.0 resta 60 fijo
  ⇒ ya FPS-independiente. `EffectStep()` metería jitter por frame-time en un init. **Igual.**
- `Render/Effects/ZzzEffectJoint.cpp:1251` `MultiUse += FACTOR` — es un **conteo entero**:
  `TargetIndex[(int)MultiUse]=j` con cap `>5`. Con factor=1.0 suma +1 por target = correcto.
  `EffectStep()` daría índices fraccionarios y corrompería el array. **Igual** (es cat-C-conteo
  6c, ya de-facto correcto en MAIN_SCENE).

**Verificación (logs, sin relanzar):**
- Puro: `tests/effects/test_effect_timing.cpp` — **6/6 casos, 15 assertions** verde
  (invariancia FPS del avance lineal + decay exp; clamp de stall; fallback frameMs<=0).
- Empírico por equivalencia: `EffectStep()` en MAIN_SCENE == `clamp(frame_ms,250)/40`, la MISMA
  cantidad que Stage 4a. `baseline/analyze_effect.py run06_s4.csv` (reusa la columna `frame_ms`
  ya capturada):
  - **Decay lineal:** NEW **25.0/s** plano @30/60/144 (disp **0.0%**) vs OLD 30/67/100 (∝FPS = bug).
  - **Decay exp:** NEW **0.0038 = 0.8²⁵** a todo FPS; OLD colapsa a ~0 (0.8^FPS) a alto FPS.
- Pendiente opcional: captura in-game de un fade/lifetime real (gateada por launch del usuario).

**Commit/tag:** `temporal/stage-06a`.
