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

## Resultado de la medición por-pasada (run12) — **P2 DESCARTADO**

`run12_pass.csv` (`analyze_perf.py … 12`), ms por pasada `[T O C E]`:

| escena | fps | terrain | objects | chars | effects |
|---|---|---|---|---|---|
| casi vacío | 573 | 0.5 | 6.4 | 2.2 | 0 |
| caminando | ~30 | **1.5** | **17** | 6 | 0 |
| multitud | ~8 | **2-3** | 7 | **80-109** | 0 |

**El terreno es insignificante: 0.5-3 ms siempre.** Un VBO de terreno ahorraría ~2 ms ⇒ **no
vale la pena.** (Estructuralmente el terreno es immediate-mode "feo" como dice el foro, pero su
costo absoluto acá es ruido.) **P2 (terreno→VBO) queda DESCARTADO.**

El cuello real es el **render de modelos BMD**, repartido en dos pasadas:
- `Objects` (props del mapa) ~17 ms caminando — mallas mayormente rígidas que igual pasan por
  el transform CPU de BMD cada frame.
- `Characters` ~80-109 ms en multitud — skinning CPU de cada personaje.

Ambas usan el mismo path `BMD::Transform`/`RenderMesh`. ⇒ **el fix es el render BMD a GPU**, que
ataca las DOS pasadas. Ver `03-bmd-gpu.md`. Effects ~0 (revertir 6a fue inocuo). Con poco en
pantalla el motor llega a 573 fps (debug) ⇒ el techo lo pone el BMD, no el resto.

## Estado

**P2 descartado por datos** (terreno ~2 ms). Pivot al render BMD a GPU (props estáticos a VBO
rígido + GPU skinning de personajes). Siguiente doc: `03-bmd-gpu.md`.

---

## REVIVIDO (jun 2026) — aprobado por el usuario pese a ROI modesto (~2ms)

Tras completar chrome-instancing + skip-skin (chars 60→11ms harness) y descartar UI/threading
como rediseños grandes, el terreno vuelve como única palanca limpia/bajo-riesgo. Datos hoy lo
confirman: terreno **~2ms crowded, ~2.5ms empty** (empty 5-7ms total → terrain VBO lo baja a
~3-4ms = 200→~300fps; NO llega a 500, falta objects+UI+loop). El usuario eligió hacerlo igual
(ahorro + calienta pipeline VBO).

### Enfoque elegido: **A+ (batched fixed-function, sin shader)**

Interceptar los `Vertex*()` (que YA tienen pos/uv/color calculados por frame en globals) para
**acumular en buckets por (textura, modo-estado)** en vez de `glBegin/glVertex`; al final de
cada pasada, **un `glDrawArrays(GL_QUADS)` por bucket**. Colapsa miles de `glBegin`+binds en
~N-textura draws. Sin shader (textura única por bucket; color per-vértice via color-array;
alpha en el canal A). Geometría/uv estáticos pero el color (`PrimaryTerrainLight`, dinámico por
`AddTerrainLight`) va en el array cada frame — igual que ya se recalcula.

### Puntos de interceptación exactos (`ZzzLodTerrain.cpp`)

- **Helper** `EmitTerrainVertex(const float pos[3], float u,float v, float r,float g,float b,float a)`:
  si `g_TerrainBatch` → `s_curBucket->push(pos,uv,rgba)` (9 floats); si no → `glTexCoord2f`+
  `glColor4f`+`glVertex3fv` (legacy idéntico).
- **Reescribir** `Vertex0..3` (1229-1255), `VertexAlpha0..3` (1292-1322), `VertexBlend0..3`
  (1364-1394) para llamar `EmitTerrainVertex` con sus mismos pos/uv/color (Vertex* → a=1;
  VertexAlpha* → a=`TerrainMappingAlpha[idx]`; VertexBlend* → rgb=alpha, a=1). Los LOD
  (`Vertex01/12/23/30/02`+alpha) quedan legacy (LOD desactivado, `lodi=1`); gate batch solo
  cuando no haya subdivisión, o rutarlos también si aparece regresión.
- **`RenderFace`/`RenderFace_After`/`RenderFaceAlpha`/`RenderFaceBlend`** (1396-1505): conservar
  el if-chain que decide el modo-estado (`EnableAlphaTest`/`DisableAlphaBlend`/`EnableAlphaBlend`
  por mapa+textura) PERO en batch mode, en vez de setear GL+`glBegin`, hacer
  `s_curBucket = &BucketFor(BITMAP_MAPTILE+Texture, modeFlag)` y llamar los 4 `Vertex`. modeFlag
  ∈ {DISABLE_BLEND, ALPHA_TEST, ALPHA_BLEND}.
- **`TerrainBatchFlush()`** nuevo: ordenar buckets por modo (DISABLE_BLEND → ALPHA_TEST →
  ALPHA_BLEND, preserva "overlay después de base"; tiles coplanares distintos → orden intra-modo
  irrelevante). Por bucket: aplicar estado, `BindTexture`, `glEnableClientState`+`glVertexPointer/
  glTexCoordPointer/glColorPointer` (stride 9*4, interleaved) + `glDrawArrays(GL_QUADS,0,n)`.
  Limpiar buckets. Llamar al final de cada `RenderTerrainFrustrum` (o tras cada pasada en
  `RenderTerrain`: normal + grass).
- **Flag** `g_TerrainBatch` = env `MU_TERRAINVBO` (default OFF → legacy intacto). Leer lazy.

### Formato de vértice del bucket
`struct V { float x,y,z, u,v, r,g,b,a; }` (9 floats). `std::unordered_map<uint64_t,Bucket>`
key = `(texture<<8)|modeFlag`. Reusar entre frames (clear, no free).

### Verificación
Harness no cubre terreno bien (login town) → validar **in-game A/B**: `MU_TERRAINVBO=0` vs `1`
mismo spot, comparar screenshot (sin regresión: agua animada, grass, alpha entre texturas,
bordes) + leer `terrain=` del log `[frame]` (esperado ~2→~0.4ms). Probar varios mapas
(Lorencia, Devias-agua, Atlans-agua, un mapa con grass). Cuidado casos especiales (Kanturu3rd,
CursedTemple, Empire, Karutan, BattleCastle agua reversa).

### Incrementos
1. Infra: helper + buckets + flush + flag (compila, flag OFF).
2. Rutar Vertex0..3 + RenderFace (base opaca) → A/B Lorencia.
3. VertexAlpha + RenderFaceAlpha (overlay) → A/B mapa con blend.
4. VertexBlend + RenderFaceBlend + agua → A/B Devias/Atlans.
5. Grass pass → A/B. 6. Medir, commit detrás de MU_TERRAINVBO.

**Estado: PLAN FIJADO, listo para implementar** (interceptación mapeada). Pendiente: ejecutar.
