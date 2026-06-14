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

### #1 — Crash de shutdown del cliente (exit 139 / SIGSEGV) — ✅ RESUELTO (commit 8465d990)
- **Síntoma:** al CERRAR el cliente, segfault (exit 139), en cada cierre desde Stage 3.
- **Causa real (localizada con cdb):** null deref en
  `std::_Hash<...FrameTimerScheduler::Timer...>::_Find_last` (ecx=0) = operación de mapa
  sobre `m_timers` ya liberado. **Fiasco de orden de destrucción estática:** otros
  globals/statics (UI widgets, buff timers) llaman `Kill()`/`SetRepeating()` desde SUS
  destructores al salir, después de que el Meyers-singleton `FrameTimerScheduler` ya se
  destruyó → mapa liberado. Pre-existente, NO derivado del desacople.
- **Fix:** heap-allocate el singleton y nunca destruirlo (leak intencional) →
  `m_timers` válido durante todo el teardown estático. Verificado con cdb: cierre limpio
  (exit 0, sin AV).

### #2 — Pose del cuerpo choppy a alto FPS (Stage 4b, P2) — ✅ RESUELTO (commit 0b1de350, tag stage-04b)
- Implementado: blend de pose entre ticks (`Render::AnimInterp`), estado prev en arrays
  paralelos (NO en OBJECT), toggle `$poseinterp` default ON. Verificado: @~60fps la pose
  render avanza 97.3% de frames vs 41.9% la cruda. Ver `04-animaciones.md` §4b.
- **Lección (regla nueva):** NO agregar estado de render al struct `OBJECT`
  (`w_ObjectInfo.h`, header incluidísimo). El 1er intento lo hizo → el cliente crasheaba
  **antes del server-select**. Usar arrays paralelos en `Render::Interpolation` (patrón
  Stage 3). Confirmado con cdb que el revert lo arregló.

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

## Debugging de crashes (cdb / WinDbg)

WinDbg instalado (winget `Microsoft.WinDbg`). El cdb x86 (para el cliente x86) está en
`C:\Users\ipera\AppData\Local\Microsoft\WindowsApps\cdbX86.exe`. Receta para capturar el
stack de un crash sin IDE (lo que localizó el #1):

```bash
export MSYS2_ARG_CONV_EXCL="*"          # no manglear /u /p
cd <Debug dir>                          # cwd = donde está Main.exe (assets/DLLs)
CDB="/c/Users/ipera/AppData/Local/Microsoft/WindowsApps/cdbX86.exe"
"$CDB" -g -G -lines -cf "<repo>/cdb-crash.txt" "$PWD/Main.exe" connect /u127.0.0.1 /p44406
```

`cdb-crash.txt` (en el root del worktree):
```
sxe -c "kvn 80;.echo ===CDB-CRASH-STACK-END===;qd" av
g
```
En cualquier access-violation (arranque, runtime o shutdown) vuelca el call stack y sale.
Necesita `Main.pdb` junto al exe (build Debug ya lo genera).

## Esquema CSV actual (12 col) — `TemporalCsvLogger`

```
t_ms, fps, hero_x, hero_y, hero_render_x, hero_render_y, hero_units_per_sec, steps, interp_alpha, frame_ms, hero_anim, hero_anim_render
  0    1      2       3          4              5                6             7        8            9        10          11
```
- `hero_x/y` = posición cruda de sim (salta @25Hz). `hero_render_x/y` = interpolada (Stage 2/3).
- `steps` = ticks de sim ese frame. `interp_alpha` = alpha de render. `frame_ms` = duración real del frame (Stage 4a).
- `hero_anim` = frame de animación del cuerpo crudo; `hero_anim_render` = interpolado (Stage 4b).
- Analizadores: `analyze.py` (velocidad), `analyze_interp.py` (interp posición render vs raw), `analyze_anim.py` (tasa de avance de animación 4a), `analyze_pose.py` (suavidad de pose 4b).
