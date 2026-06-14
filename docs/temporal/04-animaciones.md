# Etapa 4 — Animaciones (avance lógico en tick + blend de pose por frameMs)

> **Depende de:** Stages 1b/2/3 ✅. No es user-gate (pero hay decisión de alcance — ver Gate A). Verificación: **logs** (tasa de avance de `AnimationFrame` plana a todo FPS) + regresión.

## Cómo funciona la animación hoy (mapa as-found)

El frame de animación es un `float` por objeto (`o->AnimationFrame`). El **avance** y la **pose** están separados:

- **Avance** (mutación del frame): `BMD::PlayAnimation` (`ZzzBMD.cpp:397`):
  ```cpp
  *AnimationFrame += Speed * FPS_ANIMATION_FACTOR;   // + wrap/loop
  ```
- **Pose** (evaluación de huesos): `BMD::Animation` (`ZzzBMD.cpp:46`) — **ya interpola** entre keyframes por la fracción del frame:
  ```cpp
  float s1 = AnimationFrame - (int)AnimationFrame;          // fracción [0,1)
  QuaternionSlerp(q1, q2, s1, BoneQuaternion[i]);           // slerp keyframe prior→actual
  Matrix[..][3] = Position1*(1-s1) + Position2*s1;          // lerp posición de hueso
  ```
  ⇒ la pose es suave **siempre que la fracción de `AnimationFrame` avance cada render frame**.

### Quién avanza qué, y dónde

| Frame | Avance en | Path | Estado post-1b |
|---|---|---|---|
| `o->AnimationFrame` (cuerpo del char/mob) | `CharacterAnimation:2551` ← `AnimationCharacter:3706` ← `MoveCharacter:4066` | **SIM tick** | velocidad correcta (factor=1.0 ⇒ +Speed por tick @25Hz) |
| `f->AnimationFrame` (partes linkeadas: alas, armas, capas) | `RenderLinkObject:6909` | **RENDER frame** | ✗ **se acelera a alto FPS** (factor=1.0 ⇒ +Speed por frame) |

`RenderCharacter` (9013-9044) y otros `b->Animation(...)` **solo posan** (leen el frame, no lo avanzan).

## Dos problemas que dejó 1b

Antes de 1b, todo avanzaba por render frame con `FPS_ANIMATION_FACTOR = REFERENCE_FPS/FPS` ⇒ duración constante + fracción suave. 1b fijó el factor a `1.0` en MAIN_SCENE. Consecuencias:

- **P1 — Doble velocidad de partes linkeadas (correctitud).** `f->AnimationFrame` avanza `Speed` por **render** frame. A 144 FPS las alas baten ~5.8× más rápido que a 25. *(El cuerpo del char NO sufre esto: avanza en el sim tick.)*
- **P2 — Pose choppy del cuerpo (suavidad).** `o->AnimationFrame` ahora cambia solo @25Hz ⇒ `frac` congelada entre ticks ⇒ `BMD::Animation` posa el mismo frame ~5.8 veces a 144 FPS ⇒ miembros a 25 fps aunque la posición sea suave (Stage 2). Regresión directa de 1b.

## Diseño propuesto (as-built tentativo)

### Fix P1 — avance de partes linkeadas independiente del FPS

No tocar `PlayAnimation` (lo usan 39 archivos en sim y render). En su lugar, **escalar el `Speed` en el call site de render** por tiempo real:

```cpp
// pura, testeable:  delta por frame = base * (frameMs / 40ms de tick de referencia)
float Render::AnimTiming::FrameSpeed(float baseSpeed, double frameMs);   // clamp a [0, base*maxStep]
```
- `RenderLinkObject:6909`: pasar `AnimTiming::FrameSpeed(f->PlaySpeed, frameMs)` en vez de `f->PlaySpeed`.
- `frameMs` en render: exponerlo junto al alpha (`Render::Interpolation::FrameMs()`, seteado en `SceneManager` con el mismo `frameMs` del fixed-step driver).
- Resultado: avance por segundo de partes = `base*25`, constante a todo FPS. `* FPS_ANIMATION_FACTOR` (=1.0) queda inocuo.

**Test puro:** sumar `FrameSpeed` sobre 1 s a 30 vs 240 FPS ⇒ mismo total (±ε). Acceptance "misma duración total a 30 y 240 FPS".

### Fix P2 — pose del cuerpo suave (blend entre ticks)  *(decisión de alcance)*

El cuerpo avanza @25Hz; para suavizar la pose a alto FPS hay que hacer avanzar la **fracción** en render sin tocar el estado de sim — mismo patrón que la interpolación de posición (Stages 2/3):

- snapshot `prevAnimationFrame` por objeto en el sim tick (junto al snapshot de posición que ya hace `HeroInterp`/`RemoteOnTick`),
- en render, posar a `lerp(prevAnimationFrame, AnimationFrame, alpha)` (interpolación, no extrapolación: se queda en el rango ya recorrido),
- guard en cambio de acción / reset de frame (snap, como el teleport-guard).

Más invasivo: estado de frame de animación por entidad (cuerpo + partes). Mayor pago: miembros suaves a 144/2000 FPS (clave para el objetivo de "feel" a alto FPS).

## Alcance — Gate A (decisión del usuario)

- **A (correctitud sola):** solo P1. Alas/armas dejan de acelerarse. Cuerpo ya tiene velocidad correcta (sim). Helper puro + doctest + prueba por log (tasa de avance de `f->AnimationFrame` plana a 30/60/144). Bajo riesgo, chico. Análogo a 1b.
- **B (correctitud + suavidad):** A + P2 (blend de pose entre ticks). Hace que los miembros se vean suaves a alto FPS. Más estado por entidad, más riesgo. Es lo que se nota a 144+/2000 FPS.
- **A ahora, B como 4b:** partir como 1/1b — cerrar P1 con log, luego P2 aparte.

## Plan de test

- **Puro:** `AnimTiming::FrameSpeed` invariante FPS (doctest). `mu_add_test` `tests/anim`.
- **Log (P1):** extender CSV con la tasa de avance de un `f->AnimationFrame` animado (ej. alas), o instrumentar un contador de frames de animación/seg. A 30/60/144 debe quedar plano.
- **Log (P2, si va B):** CSV de `pose_frac` por render frame; con blend debe variar suave cada frame (no escalonar @25Hz).
- **Regresión:** velocidad del Hero sigue plana (no se toca la sim de movimiento).

## Riesgo / rollback

- P1: cambio acotado a `RenderLinkObject` + helper puro. Sin tocar `PlayAnimation` ni firmas.
- P2: estado por entidad en el hot path (1 float extra por objeto/parte); guard en reset.
- Rollback: `git revert` del commit de etapa (tag `temporal/stage-04`).

## Stage 4a — as-built + resultados (P1 cerrado)

Gate A → el usuario eligió **A ahora, B como 4b**. Implementado solo P1.

**As-built:**
- `Render/AnimTiming.{h,cpp}` — `FrameSpeed(baseSpeed, frameMs) = baseSpeed·clamp(frameMs)/40`, clamp a 250 ms, fallback a `baseSpeed` si `frameMs≤0` (escenas sin fixed-step). Puro, sin deps de engine.
- `Render/Interpolation.{h,cpp}` — `SetFrameMs/FrameMs` (frameMs real del frame, junto al alpha).
- `Scenes/SceneManager.cpp` — `SetFrameMs(simFrameMs)` por frame; loguea columna `frame_ms`.
- `Engine/Object/ZzzCharacter.cpp` `RenderLinkObject:6909` — pasa `AnimTiming::FrameSpeed(f->PlaySpeed, Interpolation::FrameMs())` en MAIN_SCENE (menú/char-select mantienen `f->PlaySpeed` crudo).
- CSV ahora 10-col (`+frame_ms`); analizador `baseline/analyze_anim.py`.
- **NO se tocó `PlayAnimation`** (lo usan 39 archivos): el fix vive en el call site + helper.

**Test puro:** `tests/anim/test_anim_advance.cpp` 5/5 — `FrameSpeed` invariante FPS (Σ a 30 vs 240 = base·25), clamp en stall, fallback `frameMs≤0`, speed 0.

**Prueba por log (`run06_s4.csv`, `analyze_anim.py`):** tasa de avance de animación de una parte con PlaySpeed=1, reconstruida con timings reales:

| bin FPS | OLD (pre-fix, ∝FPS) | NEW (fix) |
|---|---|---|
| ~30  | 30.3 /s  | **25.0** /s |
| ~60  | 67.3 /s  | **25.0** /s |
| ~144 | 100.1 /s | **25.0** /s |

OLD: dispersión 106% (el bug, escala con FPS). NEW: dispersión **0.0%** ⇒ avance plano ~25/s a todo FPS. Regresión de posición (Stage 2/3): velocidad 2.4% plana, sin regresión. motion 4/4, fixedstep 2/2.

**Conclusión 4a ✅** (tag `temporal/stage-04a`). Pendiente **4b (P2):** suavidad de pose del cuerpo entre ticks (miembros @25Hz → choppy a alto FPS, aún sin resolver).

## Stage 4b — as-built + resultados (P2 cerrado)

Blend de pose del cuerpo entre ticks reusando el alpha del frame. `BMD::Animation` ya
interpola por la fracción del frame; basta hacer avanzar esa fracción cada render frame
sin tocar el estado de sim.

**As-built (clave de diseño):**
- `Render/AnimInterp.{h,cpp}` — `Interpolate(prev*, cur*, alpha, enabled, prevValid) -> {frame, priorFrame, priorAction}`. Puro. Interpola `display = lerp(prevFrame, curFrame, alpha)` y **selecciona el prior keyframe** según en qué entero cae `display`, para continuidad en el borde del tick (cuando el `PriorAnimationFrame` del engine salta al cruzar un entero). Snap (devuelve cur) si: interp off, 1er frame, wrap (`cur<=prev`) o salto anómalo (`> kMaxTickStep=3`).
- **Estado prev en arrays paralelos, NO en OBJECT** — igual que la posición (Stage 3). `Render::Interpolation`: store por slot (`RemoteAnimOnTick/RemoteAnimPrev`) + store del Hero (`HeroAnimOnTick/HeroAnimPrev`).
  - ⚠️ **Lección:** el primer intento agregó campos a `OBJECT` (`w_ObjectInfo.h`, header incluidísimo). El cliente crasheaba **antes del server-select**. Localizado con cdb. Revertido a arrays paralelos → arreglado. **No tocar el struct OBJECT** para estado de render (ver `KNOWN-ISSUES.md`).
- Snapshot pre-tick: Hero en `MainSceneFixedUpdate` (junto a `HeroInterp::OnTick`); remotos en `MoveCharactersClient` (por slot, skip Hero). Ambos antes de avanzar la sim.
- Override/restore de `o->AnimationFrame/PriorAnimationFrame/PriorAction` alrededor del draw en `RenderCharactersClient` (todos incl. Hero).
- Toggle `$poseinterp on/off` (`Interpolation::SetPoseEnabled`), **default ON** (validado). CSV +cols `hero_anim` (crudo) / `hero_anim_render` (interpolado).

**Test puro:** `tests/anim/test_anim_interp.cpp` 7/7 — interp suave dentro del intervalo, selección de prior en borde de tick, snap en wrap/jump, clamp de alpha, disabled/first-frame → cur.

**Prueba por log (`run08_s4b.csv`, pose default ON, `analyze_pose.py`):** % de frames-móviles con cambio de fracción por frame:

| bin FPS | %raw-chg | %ren-chg |
|---|---|---|
| ~60 (1363 fr) | 41.9% | **97.3%** |

La pose interpolada avanza en 97.3% de los frames vs 41.9% la cruda → la pose se mueve ~2.3× más seguido que el tick de 25 Hz = suave. (run07b con toggle: ~144 → 60.0% vs 20.8%, mismo patrón.) render≠raw en 95.4% de frames (default on).

**Conclusión 4b ✅** (tag `temporal/stage-04b`). Stage 4 completo (4a correctitud + 4b suavidad). Toggle `$poseinterp` queda para A/B. Bonus de la sesión: el crash de shutdown (exit 139) quedó **localizado con cdb y arreglado** (`FrameTimerScheduler`, fiasco de orden de destrucción estática) — commit aparte.
