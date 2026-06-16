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

## ⚠️ CORRECCIÓN (revert de 6a) — la premisa del deep-dive era FALSA

> El deep-dive de arriba asumía que los efectos se actualizan **1× por frame de render** ⇒
> con factor=1.0 correrían ∝FPS. **Eso es FALSO post-1b.** Lectura del call graph (commit
> `194966bb`):

**Los `Move*` de efectos corren en el TICK fijo (25 tps), no por frame de render.**
`MoveBoids/MovePoints/MoveEffects/MoveJoints/MoveParticles` + `ThePetProcess::UpdatePets`
están dentro de `UpdateGameEntities()` (MainScene.cpp:267), que **Stage 1b movió a**
`MainSceneFixedUpdate()` (MainScene.cpp:378), driveado por el acumulador fixed-step en
`SceneManager.cpp:1072-1075`. ⇒ en MAIN_SCENE se ejecutan **exactamente 25×/seg**, y ahí
`FPS_ANIMATION_FACTOR == 1.0` significa "avanzá un tick de referencia" ⇒ `-= 1` por tick =
**25/s, ya FPS-independiente** (idéntico a pre-1b, donde el clamp daba lo mismo). *(En
LoginScene/CharacterScene los `Move*` SÍ corren por frame, pero ahí el factor sigue el clamp
⇒ también OK.)*

**Recategorización real de los ~3000 sitios `*FACTOR` en archivos de efectos:**
| Categoría | Función | Sitios | ¿Bug post-1b? |
|---|---|---|---|
| **Init one-shot** | `CreateEffect`/`CreateParticle`/`CreateJoint` | ~1598 | **NO** — corre 1× a la creación; factor=1.0 ⇒ constante |
| **TICK (25 tps)** | `MoveEffects`/`MoveParticles`/`MoveJoints`/`MovePoints`/`MoveBoids`/`UpdatePets` | ~1406 | **NO** — factor=1.0 ⇒ −1/tick = 25/s, ya FPS-indep |
| **RENDER (per-frame)** | `RenderEffects`/`RenderEffectShadows`/`RenderJoints` | ~30 | **POSIBLE** — solo si mutan estado persistente `o->` |

**6a fue una REGRESIÓN y se revirtió** (`194966bb`). Swappear sitios del TICK a `EffectStep()`
hizo que cada tick avanzara `clamp(renderFrameMs)/40` (≈0.17 @144fps) en vez de 1.0 ⇒ los
efectos decaían **~6× más LENTO** a alto FPS (regresión invertida). La verif in-game de 6a no
lo detectó porque `eff_step` se muestreaba en el **render path** (sum 25/s), mientras el decay
real ocurre en el **tick path**. Se restauraron las 8 líneas a `FPS_ANIMATION_FACTOR` en
`ZzzEffectPoint`/`GOBoid`/`CSPetSystem`.

**Se MANTIENE:** módulo `Render/EffectTiming.{h,cpp}` + `EffectTimingGlue.cpp` + tests
(`tests/effects`, 6/6) + sonda CSV `eff_step`/`eff_decay` + analizadores. El módulo es correcto
para su uso previsto (**render path**, 1×/frame), solo se aplicó a los sitios equivocados.

## Stage 6 corregido — el bug real son ~20 sitios RENDER-path con estado persistente

Sitios per-frame que mutan estado persistente `o->` (acumulan entre frames ⇒ ∝FPS con
factor=1.0). El resto de los sitios render-path usan **temporales locales** (`vPos`, `Light1`,
`Light2`, `Scroll`, `Luminosity`, `fSpeed`) recomputados cada frame ⇒ **no** son bug.

| Archivo / función | Líneas | Estado mutado | Fix |
|---|---|---|---|
| `ZzzEffect.cpp` `RenderEffects` | 18347, 18358 | `o->Angle[2] -= 0.1f*F` | `*EffectStep()` |
| `ZzzEffect.cpp` `RenderEffects` | 18652-18660 | `o->Light[i] *= pow(±fLight,F)` | `EffectDecayExp` |
| `ZzzEffect.cpp` `RenderEffectShadows` | 19105-19108 | `o->Light[i]*=pow`, `o->Scale+=0.05*F` | `EffectDecayExp`/`*EffectStep` |
| `ZzzEffect.cpp` `RenderEffectShadows` | 19199-19201, 19211, 19394 | `o->Light[i]*=pow`, `o->Angle[2]+=10*F`, `o->Scale+=0.2*F` | idem |
| `ZzzEffectJoint.cpp` `RenderJoints` | 7130 | `o->Light *= powf(0.9978,F)` | `EffectDecayExp` |
| `ZzzEffectJoint.cpp` `RenderJoints` | 7285 | `o->Velocity *= powf(1/1.1,F)` | `EffectDecayExp` |
| **NO bug (locales)** | `RenderParticles` 8985/9249 (`vPos`), `RenderJoints` 7041-7066/7300-7302 | — | dejar |

⚠️ Notas: (1) algunos `Render*` se llaman 2× por frame (pase blend / reflejo de agua) ⇒ el
estado persistente se muta 2×/frame (quirk pre-existente; el fix dt al menos lo hace
FPS-indep por llamada). (2) Mutar estado de sim (`o->Light/Angle/Scale/Velocity`) en el render
es un smell; el fix mínimo es dt, el ideal sería moverlo al tick (fuera de alcance acá).
(3) Todos son **cosméticos menores** (parpadeo/fade de luz, rotación, escala de efectos).

**Verificación correcta (cuando se haga):** sonda en el **render path** del estado `o->`
afectado (p.ej. `o->Light[0]` de un efecto fijo) + capturar a 30/60/144 ⇒ misma curva de
decay. (NO reusar `eff_step` solo, que ya se probó es la integral por-frame.)

**Estado:** Stage 6 (efectos) **re-scopeado**, pendiente Gate del usuario sobre si vale la pena
(~20 sitios cosméticos menores) vs pivotar a GPU. Módulo listo para reusar.
