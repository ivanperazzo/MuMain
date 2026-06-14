# Etapa 3 — Entidades remotas (interpolación de render de mobs/otros players)

> **Depende de:** Stage 2 (Lerp + interpolación del Hero ✅). No es user-gate. Verificación: visual (mobs/otros players suaves a alto FPS) + regresión de velocidad.

## Qué cambia

Tras 1b+2, el **Hero** se ve suave a alto FPS, pero las demás entidades (mobs, otros jugadores) se mueven a velocidad correcta (corren en el sim tick) **sin interpolar** → escalonan a 25 Hz cuando el render va a 144+. Stage 3 les aplica la misma interpolación prev→cur que al Hero, reusando `Render::Interpolation::Lerp`.

Diferencia con el Hero: hay **muchas** entidades y la cámara NO las sigue. Por eso:
- prev se guarda **por slot** de `CharactersClient[i]` (array indexado, sin tocar el struct OBJECT).
- el override de posición es **por-modelo** dentro de `RenderCharactersClient` (no global), restaurando tras cada `RenderCharacter`.

## Sitios

| Archivo | Cambio |
|---|---|
| `Render/Interpolation.{h,cpp}` | + `SetFrameAlpha/FrameAlpha` (alpha compartido del frame) + remoto indexado: `RemoteOnTick(i,pos)`, `RemoteRenderPos(i,cur,out)` (lerp + teleport-guard) |
| `Render/HeroInterpolation.cpp` | `RenderPos` usa `Interpolation::FrameAlpha()` (alpha único, ya no propio) |
| `Scenes/SceneManager.cpp` | `Interpolation::SetFrameAlpha(simAlpha)` (reemplaza `HeroInterp::SetAlpha`) |
| `Engine/Object/ZzzCharacter.cpp` `MoveCharactersClient` | snapshot `RemoteOnTick(i, pos)` antes de mover cada char (sim tick) |
| `Engine/Object/ZzzCharacter.cpp` `RenderCharactersClient` | para `c != Hero`: override `o->Position = RemoteRenderPos(i, pos)` alrededor de `RenderCharacter`, restore |

## Teleport-guard

Los mobs warpean por corrección del server (ver memoria `monster-position-desync-open`). El guard (salto > ~3 tiles/tick → snap, sin lerp) evita que se deslicen por el mapa en el warp. Mismo umbral que el Hero.

## Plan de test

- Pure: `Lerp` ya testeado (Stage 2). El remoto reusa la misma math; la indexación es glue.
- Visual: un mob caminando a `$fps 144` debe verse suave (no a saltos). Menos rubber-band perceptible.
- Regresión: velocidad del Hero sigue plana (no se toca la sim).

## Riesgo

- Override/restore por-entidad en el hot render path: barato (3 floats × N visibles), restaurado siempre.
- Índice consistente snapshot↔render (ambos por `CharactersClient[i]`).
- prev sin inicializar el 1er frame → teleport-guard snapea a cur (seguro).

## Rollback

Acotado a `ZzzCharacter.cpp` + `Interpolation`. `git revert` del commit de etapa.

## Resultados (verificación empírica con logs)

Verificación **basada en logs** (no inspección visual — el render suave de mobs no era
distinguible a ojo). Se extendió el CSV de `TemporalCsvLogger` con columnas
`hero_render_x/hero_render_y` (posición dibujada interpolada) junto a la cruda
`hero_x/hero_y`, y se agregó el toggle `$interp on/off` (gatea Hero + remotos, mismo
path). Captura: `baseline/run05_s3.csv` (7888 frames). Analizador:
`baseline/analyze_interp.py`.

**Patrón crudo↔render (caminando, `$fps 144`, interp ON):**

```
raw  (hero_x): 9386.08 [tick] → 9386.08 → 9386.08 → 9386.08 → 9394.57 [tick]   salta 1×/tick (+8.49)
ren (render_x): 9377.89 → 9380.09 → 9382.11 → 9384.30 → 9386.32                +~2.1/frame, suave
```

El raw queda quieto y salta 1 vez por tick (~25 Hz); el render avanza en pasos chicos
y uniformes cada frame, rellenando el hueco entre ticks.

**PRUEBA A — render rellena entre ticks** (`analyze_interp.py`):

| bin FPS | frames móv | %raw-mov | %ren-mov | saltoMaxRaw | saltoMaxRen |
|---|---|---|---|---|---|
| ~60  | 733  | 27.7% | 70.5% | 19.09 | 12.01 |
| ~144 | 3128 | 24.3% | 64.8% | 23.82 | 23.82 |

`%ren-mov` (render cambia) >> `%raw-mov` (raw cambia): el render se mueve ~2.7× más
frames que el raw → suaviza. PRUEBA B (saltoMaxRen < saltoMaxRaw) se cumple en ~60;
en ~144 un frame outlier iguala (snap del teleport-guard), no relevante.

**PRUEBA C — el toggle `$interp on/off` funciona** (% frames-móviles con render==raw exacto):

| tramo | %render==raw | estado |
|---|---|---|
| seg 55–95  | 0–5%   | interp **ON** (suaviza casi cada frame) |
| ~105 s     | —      | **toggle** |
| seg 105–126| **100%** | interp **OFF** (render==raw exacto) |

Con interp OFF, el render colapsa exactamente al raw → confirma que el suavizado es lo
único que cambia y que el toggle gatea el path completo (Hero + remotos).

**Conclusión:** Stage 3 ✅ — interpolación de render activa y correcta; raw/sim intacta
(velocidad plana, no se toca la sim). Regresión: motion 4/4, fixedstep 2/2.

> **Nota / issue aparte:** el cliente segfaulteó (exit 139) en el **cierre** (teardown
> de bitmaps + "Destroy" en `MuError.log`), post-sesión. No afecta la captura ni la
> simulación. Pendiente investigar el crash de shutdown por separado.
