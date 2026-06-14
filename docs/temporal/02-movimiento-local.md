# Etapa 2 — Movimiento local (cat A → integración por tick + interpolación de render)

> **Estado:** Deep-dive de investigación. **GATE A** — requiere aprobación antes de tocar código de engine.
> **Depende de:** Stage 1b (fixed-step en MAIN_SCENE ✅). **Empírico (E1) diferido** al server (ver `authoritative-server-empirical-deferred`).

---

## Reframe importante (qué cambia DE VERDAD post-1b)

El movimiento del Hero/personajes es `MoveCharacterPosition()`:
```cpp
VectorAddScaled(o->Position, Velocity, o->Position, FPS_ANIMATION_FACTOR);  // pos += vel * factor
o->Timer += 0.15f * FPS_ANIMATION_FACTOR;
```
Es **cat A** (lineal escalada por factor). Tras Stage 1b esto corre dentro de `MainSceneFixedUpdate()` a 25 tps con `factor=1.0` → `pos += vel` por tick → `vel*25/seg`, que es la velocidad de balance. **La VELOCIDAD ya está correcta.** E1 (distancia/seg plana a 30↔240) ya debería cumplirse por 1b solo.

Lo que falta y **Stage 2 aporta** es **SUAVIDAD**: con la sim a 25 Hz, a 144 FPS de render el Hero se dibuja ~6 frames en la misma posición y luego "salta" → stutter. Stage 2 agrega **interpolación de render**: guardar la posición del tick anterior y el actual, y dibujar en `lerp(prev, cur, alpha)` con el `alpha` de `SimulationClock` (ya disponible como `simAlpha` en RenderScene).

Secundariamente: formalizar la integración como **función pura** (`IntegratePosition`) — la extracción ES la mejora (testeable), aunque con dt fijo la invariancia 30↔240 sea ahora trivial.

---

## Sitios afectados

| Archivo:línea | Símbolo | Rol | Cambio | Cat |
|---|---|---|---|---|
| `Engine/Object/ZzzCharacter.cpp:6363` | `MoveCharacterPosition` | `pos += vel * factor` (tick) | Llamar a `Movement::IntegratePosition` puro; **snapshot prev→cur** de la posición | A |
| `Engine/Object/ZzzCharacter.cpp:6276` | `CharacterMoveSpeed` | velocidad por frame (units) | Sin cambio de valor (queda como vel por tick); documentar unidades | A |
| `Engine/Object/ZzzCharacter.cpp:6371` | `VectorAddScaled(...factor)` | integración | Reemplazo por integración pura (factor ya = 1.0 en MAIN_SCENE) | A |
| OBJECT (struct, `ZzzObject.h`) | `Position` | estado | Agregar `RenderPrevPosition[3]` (snapshot del tick previo) | — |
| Render del personaje (lee `o->Position`) | `RenderCharacter`/draw | dibujo | Dibujar en `lerp(RenderPrevPosition, Position, alpha)` para el Hero (y opcional otros) | — |

> **A mapear durante implementación:** los call-sites de render que leen `o->Position` para posicionar el modelo del Hero. Hoy el render usa `o->Position` directo; Stage 2 interpone el lerp. El alcance inicial = **solo Hero** (cat A local); entidades remotas son Stage 3.

---

## Comportamiento actual → objetivo

- **Hoy (post-1b):** `Position` se actualiza 25×/seg; el render dibuja `Position` crudo a su FPS → velocidad correcta pero salto visible a alto FPS.
- **Objetivo:** por tick, `RenderPrevPosition = Position; Position = IntegratePosition(Position, vel, FIXED_DT)`. En render, dibujar `lerp(RenderPrevPosition, Position, alpha)`. Resultado: trayectoria idéntica (misma velocidad) pero **suave** a cualquier FPS.

---

## Lógica a extraer a puro

Módulo nuevo `Render/Interpolation` (namespace `Render::Interpolation`) — o `Motion::` para la integración. Propuesta:

```cpp
namespace Render::Interpolation
{
    // alpha in [0,1]; componente a componente.
    void Lerp(const float prev[3], const float cur[3], float alpha, float out[3]);
}

namespace Motion
{
    // pos_out = pos + vel * (dtMs / referenceTickMs). Con dtMs==referenceTickMs
    // (40ms) equivale a pos += vel (un tick). Pura, sin deps de engine.
    void IntegratePosition(const float pos[3], const float vel[3],
                           double dtMs, float out[3]);
}
```

> Nota de capa: `Lerp` es render; `IntegratePosition` es game-logic/motion. Mantenerlas en namespaces separados aunque vivan cerca (CODING_RULES: separación de capas).

---

## Plan de test (doctest, offline)

`tests/motion/test_motion_integration.cpp`:
- `IntegratePosition`: misma distancia total acumulada con dt=40 (1 paso) vs dt=8×5 (5 pasos) → invariancia (±epsilon).
- `IntegratePosition`: velocidad cero → posición sin cambio.
- `Lerp`: alpha=0 → prev; alpha=1 → cur; alpha=0.5 → punto medio; por componente.
- (Sanidad) integrar N ticks a velocidad v da `N*v*dt/40` exacto.

Registrar con `mu_add_test` en `tests/motion/CMakeLists.txt` + `add_subdirectory(motion)` en `tests/CMakeLists.txt`.

---

## Riesgo + mitigación

1. **Snapshot prev/cur (medio).** `RenderPrevPosition` debe setearse **una vez por tick, antes** de integrar, no por frame de render. Si hay 0 ticks en un frame de render, `alpha` crece y el lerp extrapola hacia `cur` ya alcanzado — correcto (alpha<1 siempre porque droppedDebt resetea). Cuidar el caso de teleport/spawn: al saltar posición, igualar `RenderPrevPosition=Position` para no interpolar a través del mapa.
2. **Solo Hero primero (bajo).** Acotar a Hero evita tocar remotas (Stage 3) y mobs. Reduce superficie.
3. **Empírico diferido (alto, aceptado).** La suavidad SOLO se verifica en runtime (MAIN_SCENE) → diferido al server. Pure tests cubren la matemática; la integración queda code-complete sin probar visualmente. Riesgo asumido (estrategia "apilar").
4. **Terreno Z.** `MoveCharacterPosition` recalcula `Position[2]` por terreno tras integrar X/Y. El lerp de render debería interpolar X/Y y dejar Z por terreno (o lerp completo; decidir en impl). Mantener Z = terreno del punto interpolado evita hundir/flotar el modelo.

---

## Criterio de éxito

- `IntegratePosition`/`Lerp` puros, testeados (invariancia + endpoints). `ctest -R motion` verde.
- `cmake --build` verde.
- **(Diferido)** E1 plano 30↔240 (ya esperado por 1b) + Hero visualmente suave sin jitter a 144, sin stutter de 25 Hz — verificar cuando el cliente conecte.

## Rollback

Acotado a `ZzzCharacter.cpp`, el struct OBJECT (+1 campo) y los call-sites de render del Hero. `git revert` del commit de etapa. El campo `RenderPrevPosition` es aditivo; sin cambios de persistencia/protocolo.

---

## Decisiones para el Gate A

1. **Alcance de interpolación:** ¿solo Hero en Stage 2 (recomendado; remotas en Stage 3), o Hero + personajes locales ya?
2. **Z en el lerp:** ¿interpolar solo X/Y y dejar Z=terreno del punto interpolado (recomendado), o lerp de los 3 componentes?
3. **Ubicación del módulo:** `Render/Interpolation` (Lerp) + `Motion/` (IntegratePosition) — ¿OK separar, o agrupar en uno?
