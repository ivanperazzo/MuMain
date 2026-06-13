# Baseline temporal — procedimiento de captura

Estos CSV son el **baseline** contra el que se verifica cada etapa (E1–E6). Se
capturan con el cliente instrumentado de Stage 0 (`TemporalCsvLogger`), ANTES de
cablear el fixed-step (Stage 1b), para fijar el comportamiento "tal cual está hoy".

## Prerrequisitos

1. **Build instrumentado** — el cliente debe incluir el `TemporalCsvLogger`
   (Stage 0 ya mergeado). `Main.exe` en
   `out/build/windows-x86/src/Debug/Main.exe`.
2. **Servidor OpenMU corriendo** y escuchando en el puerto del cliente
   (`localhost:44406` por defecto). Sin servidor el cliente nunca entra a
   `MAIN_SCENE` y el probe no registra nada (solo loguea en MAIN_SCENE con Hero
   válido).
3. Cuenta de prueba (`test0`…`test9`, pass = usuario) con un personaje.

## Flag

El logging se activa con la env var `MU_TEMPORAL_CSV`:

```powershell
# escribe el archivo indicado
$env:MU_TEMPORAL_CSV = "I:\MuOnline\MuMain-temporal\docs\temporal\baseline\fps60.csv"
```

Con el flag sin setear, el cliente no escribe nada (costo cero).

## Protocolo de captura (repetir por cada FPS)

Capturar a **30, 60 y 144 FPS**. Para cada uno:

1. Setear el cap de FPS. El cliente lo expone vía `SetTargetFps(double)`
   (`Scenes/SceneManager.cpp:176`); en el juego se ajusta por opciones de video.
   Para forzarlo determinísticamente conviene un cap fijo en config/opciones.
2. Setear `MU_TEMPORAL_CSV` al archivo de salida (`fps30.csv`, `fps60.csv`,
   `fps144.csv`).
3. Lanzar el cliente y entrar al mundo con el personaje de prueba.
4. **Mover al Hero una distancia fija y repetible**: caminar en línea recta entre
   dos puntos conocidos del mapa (idealmente Lorencia, terreno plano), ida y
   vuelta varias veces, ~20–30 s de movimiento continuo.
5. Cerrar el cliente. El CSV queda en `docs/temporal/baseline/`.

> Columnas: `t_ms,fps,hero_x,hero_y,hero_units_per_sec,steps,interp_alpha`.
> `steps`/`interp_alpha` = 0 hasta Stage 1b. `hero_units_per_sec` es el agregado
> rodante del `MovementProbe`; las columnas crudas `t_ms,hero_x,hero_y` permiten
> recomputar la velocidad real de los tramos en movimiento offline.

## Criterio (qué demuestra el baseline)

Hoy (sin fixed-step) se ESPERA que `hero_units_per_sec` **difiera** entre 30 y
144 FPS para los sitios cat E (speedhack), y/o que a 10 FPS la sim caiga por el
clamp (E2). Ese delta ES el problema que Stage 1b debe eliminar: tras 1b, las
mismas mediciones deben quedar planas ±2% (E1) y sostenerse a 10 FPS (E2).

## Análisis

Para cada CSV, la velocidad de un tramo en movimiento = `dist(p_i, p_j) /
((t_j - t_i)/1000)` sobre filas donde el Hero se mueve. Comparar el promedio de
tramos entre `fps30/fps60/fps144`. La math del agregado está testeada en
`tests/diagnostics/test_movement_probe.cpp` (invariancia frame-rate).
