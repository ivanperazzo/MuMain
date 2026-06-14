# Etapa 1b — Resultados de verificación (E1)

> Verificación empírica **post-cambio** de Stage 1b (fixed-step en MAIN_SCENE). Cliente temporal conectado a un OpenMU local (connect 44406 → game 55901/55902), cuenta `test1`, caminata manual del Hero a 30 / 60 / 144 FPS (`$vsync off` + `$fps N`). Captura: `docs/temporal/baseline/run01.csv` (15.325 frames). Análisis: `docs/temporal/baseline/analyze.py`.

## No hay baseline pre-1b en este build

Stage 1b ya está compilado en el binario probado, así que no se midió el "antes" (speedhack) para contrastar. La prueba de corrección es la **planitud** de la velocidad y de la tasa de ticks a distintos FPS: si el mundo dependiera del render, ambas escalarían con el FPS.

## Resultados

**(1) Tasa de simulación por bin de FPS** — prueba directa del fixed-step. Objetivo ~25 tps a todo FPS; si fuera per-frame sería ~30/60/144.

| FPS render | ticks/seg | muestra |
|---|---|---|
| ~30 | **24.0** | 21.7 s, 520 ticks |
| ~60 | **24.4** | 236.6 s, 5774 ticks |
| ~144 | **31.1** | 26.6 s, 828 ticks |

El mundo avanza a ~24–31 tps **sin importar** que el render vaya a 30, 60 o 144. No escala con el FPS → el speedhack de movimiento está cortado.

**(2) Velocidad real del Hero** (ventanas de 0.5 s = dist total / tiempo total, que promedia los saltos discretos por tick). Objetivo: plana.

| FPS render | velocidad media | mediana | n ventanas |
|---|---|---|---|
| ~30 | **287.9 u/s** | 288.0 | 23 |
| ~60 | **283.2 u/s** | 288.1 | 114 |
| ~144 | **300.9 u/s** | 306.5 | 37 |

**Dispersión entre bins: 6.1 %** → plana dentro del ruido de una captura manual. **E1 pasa.**

## Diagnóstico que confirma el mecanismo

De 9.268 frames con `steps==0` (ningún tick fijo ese frame), el Hero se movió en **0**. Es decir, el Hero **solo se desplaza cuando corre un tick fijo**, nunca en el render path. Distribución de steps/frame: `{0: 9269, 1: 6054, 5: 2}` (mayormente 0–1 por frame, como debe ser a FPS ≥ tps).

## Trampa de medición registrada

La primera métrica (promedio de **velocidad por-frame** = dist/dt de cada frame) dio una dispersión falsa de **115 %** (parecía que la velocidad escalaba con el FPS). Es un artefacto: con movimiento a saltos discretos por tick, un frame corto de alto FPS que justo tuvo 1 tick muestra `vel/dt_chico` = velocidad inflada, y el promedio sobre-pesa esos frames-salto. La medición correcta es por ventana de tiempo (o tasa de ticks). `analyze.py` quedó con la versión correcta.

## Pendiente (captura más limpia, opcional)

- E2 (cap a 10 FPS, sin caída de velocidad) no se midió aún — `$fps 10` y confirmar que sigue ~285 u/s.
- Apretar el 6.1 %: caminata más larga y a velocidad estable por bin; el bin ~144 (corto, n=37) es el que más ruido mete (31 tps / 301 u/s).
- Cuando exista el auto-walk de debug, todo esto se vuelve repetible y automático.
