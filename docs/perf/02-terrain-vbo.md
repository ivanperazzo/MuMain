# P2 — Terreno a VBO — Deep-dive + plan

> Objetivo: sacar el terreno del immediate-mode (CPU manda vértice a vértice cada frame) a
> geometría residente en GPU. Calienta el pipeline moderno (VBO/VAO, y luego shaders) que P3
> (GPU skinning) también necesita. **Pero el terreno es más complejo de lo que parecía** — ver
> abajo. Decisión de alcance = Gate A.

## Modelo de datos (`Render/Terrain/ZzzLodTerrain.cpp`)

Grid `TERRAIN_SIZE × TERRAIN_SIZE` (256²). Arrays por celda:
- `BackTerrainHeight[]` — altura (Z). **Estático** (cargado del mapa; solo el editor lo cambia).
- `TerrainLight[]` — lightmap base (JPEG). Estático.
- `BackTerrainLight[] = TerrainLight × Luminosity` — `g_Luminosity` pulsa por frame (sin()).
- `PrimaryTerrainLight[]` = back + luces dinámicas (antorchas/efectos, `AddTerrainLight`). **Per-vértice DINÁMICO cada frame** → es el `glColor3fv` por vértice.
- `TerrainMappingLayer1/2[]` + `TerrainMappingAlpha[]` — capas de textura por celda + blend.
- `TerrainGrassTexture/Wind[]`, `TerrainWall[]` — grass animado, colisión.

## Render path actual (immediate-mode, 2 pasadas)

`RenderTerrain` → `RenderTerrainFrustrum` itera **bloques 4×4 dentro del frustum**
(`FrustrumBoundMinX/MaxX…`, ya hay culling) → `RenderTerrainBlock` → `RenderTerrainTile`
(`lodi=1` hardcoded, **LOD desactivado**) → `RenderTerrainFace` por tile:
- Decide textura(s) de la celda: si alpha lleno → solo Layer2; si no → Layer1 + (opcional)
  Layer2 alpha-blended. Casos especiales: **agua** (texturas 5/11, animada), **grass** (quad
  extra con viento), PK field, Blood Castle, Doppelganger, Karutan, 7th Atlans.
- `FaceTexture()`/`BindTexture()` → **cambia textura por tile**.
- `glBegin(GL_QUADS)` + 4× (`glTexCoord2f`, `glColor3fv(PrimaryTerrainLight)`, `glVertex3fv`).
- **Segunda pasada** `RenderTerrainFace_After` (más blends/capas).

⇒ Por frame: por cada tile visible → 1+ `glBegin` + cambio de textura + 4 vértices a mano, ×2
pasadas. Es exactamente lo que el foro (Feche) marcó como "lo más sin optimizar".

## Lo difícil para VBO

1. **Textura variable por tile** ⇒ batchear obliga a atlas/array de texturas + shader que
   haga el blend Layer1/Layer2/alpha (no se puede en fixed-function de una sola pasada).
2. **Color por-vértice dinámico** (`PrimaryTerrainLight` cambia cada frame por Luminosity +
   luces) ⇒ o se re-sube un VBO de color por frame, o se mueve el lighting a un shader
   (lightmap estático como atributo + Luminosity uniform + luces dinámicas como uniforms).
3. **Casos especiales**: agua animada, grass con viento (vértices se desplazan), alpha, mapas
   con reglas propias. Hay que preservarlos sin regresión visual.
4. **Geometría estática** (alturas) ⇒ esto SÍ es fácil: VBO una vez.

## Enfoques (Gate A)

- **A — VBO mínimo, misma estructura (bajo riesgo, ganancia modesta).** Geometría+UV base en
  VBO estático; color en VBO dinámico re-subido por frame (1 memcpy bulk vs miles de
  `glColor`); seguir 1 draw por tile (la textura por tile lo obliga) pero con
  `glDrawArrays`/elements en vez de `glBegin`. Fixed-function compat, **sin shaders**. Elimina
  el overhead per-vértice; NO baja el nº de draws ni los cambios de textura. Ganancia parcial.
- **B — Batched + shader (ganancia grande, alto esfuerzo/riesgo).** Atlas/array de texturas,
  fragment shader con blend de capas + lighting (lightmap atributo + Luminosity uniform), un
  VBO/IBO grande del terreno visible, **pocos draws**. Requiere contexto GL 3.3+ y sistema de
  shaders (la misma infra que P3). Reescritura con todos los casos especiales (agua/grass/
  mapas) reimplementados en shader. Es lo que da el salto real, y deja la base para P3.

## Antes de elegir: medir el desglose por pasada (HECHO la infra)

El CSV ahora loguea `terrain_ms/objects_ms/chars_ms/effects_ms` por frame (reusa el
`FrameProfiler` existente, commit d88f0673). **Próximo paso: capturar vacío + multitud y leer
cuánto es terreno** vs personajes/efectos. Esto decide si P2 vale la inversión:
- Si en multitud el terreno es chico vs personajes (probable, dado P0b: chars dominan) ⇒ P2 da
  poca ganancia en el escenario que importa ⇒ conviene saltar a P3 (GPU skinning) y usar el
  terreno solo como calentamiento del pipeline (enfoque B compartido con P3).
- Si el terreno pesa fuerte aun en vacío (el piso ~13 ms) ⇒ P2 (enfoque A o B) baja el piso.

## Verificación (logs)

`analyze_perf.py` ya imprime `[T O C E]` ms por segmento. P2 = bajar `terrain_ms` a 30/60/144
sin regresión visual. Captura antes (baseline) y después por enfoque.

## Estado

Deep-dive hecho; infra de medición por-pasada lista. **Gate A pendiente:** (1) capturar el
desglose por-pasada (vacío+multitud) para cuantificar el terreno, luego (2) elegir A vs B vs
re-priorizar a P3.
