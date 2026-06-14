# Desacople temporal — Issues abiertos y riesgos derivados

> Registro vivo de **errores potenciales derivados del desacople** (Stages 1b–4a).
> Leer antes de tocar animación / física / efectos / cámara, y al perseguir un bug
> de "algo va más rápido/lento según el FPS". Actualizar al cerrar cada ítem.

## El invariante que lo explica todo

Stage 1b fijó **`FPS_ANIMATION_FACTOR ≡ 1.0` en `MAIN_SCENE`** (`ZzzAI.cpp` `CalcFPS`).
Fuera de MAIN_SCENE (menú, char-select, login) sigue siendo el clamp original
`REFERENCE_FPS/FPS` (REFERENCE_FPS=25, `ZzzAI.h:11`).

Históricamente `FPS_ANIMATION_FACTOR` ERA el mecanismo de compensación de FPS: como
todo corría 1×/frame de render, multiplicar por `REFERENCE_FPS/FPS` mantenía la
duración constante. Al fijarlo a 1.0 y mover la sim a 25 tps fijos, ese mecanismo
**desapareció dentro de MAIN_SCENE**. Consecuencia:

> Todo sitio en MAIN_SCENE de la forma `X * FPS_ANIMATION_FACTOR`, `pow(k, FACTOR)`
> o `v += FACTOR` ahora vale `X*1`, `pow(k,1)=k`, `v+=1`. El factor **no hace nada**.

Dos resultados según la **cadencia** de ese sitio:

- **Sitio SIM-path** (corre 1×/sim tick, dentro de `MainSceneFixedUpdate` → `MoveCharacter`/AI/etc.): ahora avanza a tasa fija 25/s = **correcto** (es la tasa de referencia). ✅
- **Sitio RENDER-path** (corre 1×/frame de render, dentro del draw): ahora avanza 1×/frame = **acoplado al FPS** = el juego/efecto/decay se acelera a alto FPS. ❌ **Esta es la clase de "errores futuros derivados".**

Stage 4a arregló UN sitio render-path (avance de animación de partes linkeadas).
Quedan más, listados abajo, repartidos por etapa.

## Inventario de `FPS_ANIMATION_FACTOR` (~206 sitios, 39 archivos)

| Patrón | Sitios | Qué hace | Riesgo si es render-path |
|---|---|---|---|
| `* FACTOR` / `* static_cast<float>(FACTOR)` | 168 | escala velocidad de animación / delta de movimiento | avance/movimiento ∝ FPS |
| `pow(k, FACTOR)` / `powf` | 24 | decay/blend exponencial por frame (luz, alpha, escala) | decae más rápido a alto FPS |
| `+= FACTOR` | 14 | contadores (`c->Dead`, timers de estado) | contador corre ∝ FPS |
| `MIN(dDeltaTick, 200*FACTOR)`, `Move(0.025*FACTOR)` | 2 | paso de física | física ∝ FPS |

### Por subsistema → etapa dueña

| Subsistema (archivos) | Sitios | Cadencia probable | Estado / etapa |
|---|---|---|---|
| **Personaje** `ZzzCharacter.cpp` | 36 | mixto | **4a ✅** arregló `RenderLinkObject:6909` (partes). Cuerpo avanza en sim (OK velocidad). Resto: revisar en 4b/8. |
| **AI / boids** `GOBoid.cpp`, `ZzzAI.cpp` | 43 | sim (movimiento de mobs) | probable OK (sim tick). **Verificar** que GOBoid corre en sim, no en draw. |
| **Objetos** `ZzzObject.cpp` | 16 | mixto | revisar (Stage 6/8) |
| **Efectos** `ZzzEffectParticle/Point/Pointer/Sprite.cpp` | ~22 | **render-path** (partículas) | ❌ alto riesgo → **Stage 6** (física de efectos) |
| **Física** `PhysicsManager.cpp`, `SceneManager.cpp` (`Move(0.025*FACTOR)`, `MIN(dDeltaTick,200*FACTOR)`) | ~15 | render-path | ❌ → **Stage 6** |
| **Cámara** `CameraMove.cpp`, `DefaultCamera.cpp`, `FreeFlyCamera.cpp` | ~10 | **render-path** (travel/zoom) | ❌ cinemáticas duran distinto por FPS → **Stage 5** (user-gate) |
| **Mapas/eventos** `GM_*.cpp` (Raklion, Kanturu, Crywolf, Empire, Hellas, Aida, Doppel…) | ~80 | mixto (anim de NPCs de evento, decay de luz) | revisar por sitio; muchos `pow(k,FACTOR)` de luz son render-path → **Stage 6/8** |
| **Agua** `CSWaterTerrain.cpp` | 2 | render-path (anim de agua) | scroll de agua ∝ FPS → **Stage 6** |

> El conteo por archivo (top): ZzzCharacter 36, GOBoid 36, ZzzObject 16, ZzzEffectParticle 14, GMCrywolf1st 12, PhysicsManager 12, GMEmpireGuardian1 9, GM_Kanturu_1st 9, GMUnitedMarketPlace 8, GMEmpireGuardian3 8, GM_Raklion 8, GMSwampOfQuiet 7, GM_Kanturu_3rd 7, ZzzAI 7, GMHellas 5, GMAida 5, CameraMove 5, …

### Cómo auditar un sitio nuevo (receta)

1. ¿Está bajo `MAIN_SCENE`? Si no → sin cambios (mantiene el clamp). Fin.
2. ¿La función corre en `MainSceneFixedUpdate` (sim) o en el draw (`MainScene(hDC)`)?
   - SIM → factor=1.0 es correcto (tasa fija). Dejar; se limpia en Stage 8.
   - RENDER → **bug potencial**: el sitio perdió compensación. Portar a tiempo real
     (`Render::Interpolation::FrameMs()` + helper puro estilo `AnimTiming::FrameSpeed`,
     o `pow(k, frameMs/40)` para decays), con test puro de invariancia FPS + log.

## Issues abiertos

### #1 — Crash de shutdown del cliente (exit 139 / SIGSEGV) — ABIERTO
- **Síntoma:** al CERRAR el cliente, segfault (exit 139). Reproducible en cada cierre
  de las corridas de prueba (Stage 3 y 4a).
- **Evidencia:** `MuError.log` termina en teardown de bitmaps (`Bitmap Data\... RefCount`)
  + línea `Destroy`. El crash es en el path de destrucción, post-sesión.
- **Impacto:** NINGUNO sobre la sesión ni la captura de CSV (la data se escribe completa
  antes del cierre). Molesto, no bloqueante.
- **No confirmado** que derive del desacople (puede ser pre-existente). Investigar aparte:
  correr bajo debugger, ver orden de destrucción de singletons/recursos GL.

### #2 — Pose del cuerpo choppy a alto FPS (Stage 4b, P2) — PENDIENTE
- `o->AnimationFrame` del cuerpo avanza en sim @25Hz; `BMD::Animation` interpola por la
  fracción, pero la fracción queda congelada entre ticks ⇒ miembros a 25 fps aunque la
  posición sea suave. Plan: blend de pose entre ticks (snapshot `prevAnimationFrame` por
  entidad + lerp por alpha). Ver `04-animaciones.md` §P2. Decidido: 4b aparte.

### #3 — Sitios render-path con factor=1.0 sin compensar — PENDIENTE (por etapa)
- Efectos/partículas, física, cámara cinemática, agua, decays de luz de mapas. Ver
  inventario arriba. Se cierran en Stages 5 (cámara), 6 (efectos/física), 8 (limpieza).
- **Hasta entonces:** a alto FPS estos pueden ir más rápido / decaer antes. Si aparece un
  bug "tal efecto/cámara va raro según FPS", empezar por acá.

### #4 — `analyze.py` (baseline) índice de columna desactualizado — MENOR
- `analyze.py` métrica (1) lee `steps` en la col 5, pero el esquema creció a 10 col
  (`steps` ahora col 7). La métrica (2) (velocidad por ventana, cols x/y) sigue válida.
  Arreglar el índice si se vuelve a usar la métrica (1). `analyze_anim.py` ya usa el
  esquema 10-col correcto.

## Esquema CSV actual (10 col) — `TemporalCsvLogger`

```
t_ms, fps, hero_x, hero_y, hero_render_x, hero_render_y, hero_units_per_sec, steps, interp_alpha, frame_ms
  0    1      2       3          4              5                6             7        8            9
```
- `hero_x/y` = posición cruda de sim (salta @25Hz). `hero_render_x/y` = interpolada (Stage 2/3).
- `steps` = ticks de sim ese frame. `interp_alpha` = alpha de render. `frame_ms` = duración real del frame (Stage 4a).
- Analizadores: `analyze.py` (velocidad), `analyze_interp.py` (interp render vs raw), `analyze_anim.py` (tasa de avance de animación).
