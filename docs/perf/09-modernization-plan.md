# 09 — Plan de modernización del renderer (post-threading)

> **Estado:** plan estratégico. Escrito mientras el track de threading actual sigue en WIP.
> Iterar/ejecutar una vez cerrado threading. No arrancar antes de tener el **profiler+overlay** (Etapa 0).
> Complementa: `00-MASTER-PLAN.md`, `02-terrain-vbo.md`, `03..05` (BMD GPU/instancing), `06` (UI text-cache), `07` (char collect), `08` (jobs audit).

## Discrepancia a verificar primero
Una auditoría de exploración sobre la rama `main` reportó "0 instancing / 0 GPU skinning / 100% legacy". Eso es la **baseline pre-perf**: el trabajo `MU_GPUBLENDINST`/`MU_GPUWAVEINST`/`MU_GPUBLENDMESH`/skip-skin/chrome-variants ya vive en `temporal/integration` (char pass 16→7-8ms). Antes de ejecutar este plan, confirmar qué rama está activa. Terreno/objetos/UI **sí** siguen legacy.

---

## 1. Diagnóstico estratégico

Cuello = **CPU-bound en submission GL**, no cómputo paralelo ni GPU. GPU ociosa en peor caso.

| Estado | Qué | Evidencia |
|---|---|---|
| Ya optimizado | Build chars 6.9→1.7ms (workers); char meshes 100% instanced (16→7-8ms); UI text-cache 5.5→1.38ms; blend/wave→GPU buckets | docs 04-07 |
| Domina ahora | ~12ms restantes: **terreno (sin VBO)**, objetos, efectos/partículas, overhead frame (~9ms), swap | RenderTerrain legacy |
| No tocado | Terrain VBO, objetos batching, GPU skinning TOTAL (chars aún bone/anim CPU ~10ms crowd), frame pacing/cap | — |

- **fps headline engañan:** escena vacía mide piso (overhead fijo + GPU sin carga) → 200-300fps. Caso pesado (100 chars) = ~52fps gobierna sensación real.
- **Amdahl:** ya paralelizada la fracción "build chars" (~36%→~9%). Speedup de paralelizar *más* limitado por la parte serial: submission GL es serial (1 contexto, 1 hilo driver). Techo threading casi tocado.
- **"Hacer menos" > "más hilos":** costo dominante = overhead por-draw-call + re-upload per-frame (client-side arrays re-DMA cada frame; immediate `glVertex3fv` per-vértice = lo peor). VBO estático + batching + GPU skinning **borran** trabajo, no lo reparten.

Principios: ↓draw calls (menos validación/sync driver), batching (↓state changes), instancing (N copias→1 draw), ↓tráfico CPU→GPU (VBO residente vs re-DMA).

---

## 2. Mapa de la codebase (aterrizado, file:line)

| Subsistema | Ubicación | Estado | Acción |
|---|---|---|---|
| Main loop | `App/Platform/Windows/Winmain.cpp:980 MainLoop()` | SDL pump + tick | frame pacing/cap aquí |
| Render orquestador | `Scenes/SceneManager.cpp:1044 RenderScene()` | selecciona escena, swap | timers por-pass |
| Frame mundo | `Scenes/MainScene.cpp:396 RenderGameWorld()` | llama todos los pases | insertar `ScopedPass` |
| **Terreno** | `Render/Terrain/ZzzLodTerrain.cpp:3065 RenderTerrain()` | LOD+frustum, **SIN VBO** | **PR: VBO** |
| Objetos | `Engine/Object/ZzzObject.cpp:3274 RenderObjects()` + `:3492 _AfterCharacter()` | por-objeto | instancing props |
| Chars | `Engine/Object/ZzzCharacter.cpp:11317 RenderCharactersClient()` | parcial GPU | GPU skinning total |
| Efectos | `Render/Effects/ZzzEffect.cpp:18076 RenderEffects()` | blend mesh, parcial GPU | — |
| Partículas | `Render/Effects/ZzzEffectParticle.cpp:8895 RenderParticles()` | immediate probable | VBO dinámico |
| Items piso | `Engine/Object/ZzzObject.cpp:6407 RenderItems()` | por-item | distance cull + batch |
| UI | `UI/NewUI/NewUISystem.cpp:1826 CNewUISystem::Render()` → `NewUIManager` (ASUNCIÓN nombre) | text-cache hecho; 4 sorts/3 copias/frame | rediseño retained (diferido) |
| Swap | `Winmain.cpp:188 PlatformSwapBuffers()` → `SDL_GL_SwapWindow` | vsync gobierna | medir bloqueo swap |
| Skinning | `Render/Models/ZzzBMD.cpp:46 Animation()`, `:165 Transform()`; arrays `BoneTransform/VertexTransform/NormalTransform` | **CPU puro** | vertex shader |
| Draw submission | `ZzzBMD.cpp:1278 RenderMesh()` (client arrays) + `:1646 RenderMeshAlternative()` (glBegin) | híbrido, **sin VBO** | VBO/VAO |
| Texturas | `Render/Sprites/GlobalBitmap.cpp:379 LoadImage()`, `:576 OpenJpegTurbo`, `:691 OpenTga`; cache TTL 1500ms | sin atlas, sin glTexSubImage | atlas + dedup |
| BMD loader | `ZzzBMD.cpp:2683 Open2()` | carga binaria | dedup modelos |
| Culling | `Camera/Frustum.cpp TestSphere/TestAABB/TestPoint2D`; `Engine/Object/CullingConstants.h` | frustum+distance SÍ, **occlusion NO** | tune radios |
| Material | `ZzzOpenglUtil.cpp:157 BindTexture`, flags `RENDER_*` | **fixed-function, sin GLSL** | shaders al modernizar |

---

## 3. Hipótesis priorizadas (impacto × 1/dificultad-validación)

| # | Hipótesis | Impacto | Validación | Corrección |
|---|---|---|---|---|
| H1 | **Terreno sin VBO** (re-sube tiles/frame) | **Alto** | Fácil (timer+draws) | VBO estático por chunk |
| H2 | **Skinning CPU residual** crowd (~10ms) | **Alto** | Media (profilear Transform 100ch) | GPU skinning (bones uniform/UBO + VS) |
| H3 | **Sin frame cap/pacing** | Medio | Fácil (log dt) | cap + vsync correcto |
| H4 | **Objetos sin instancing** | Medio-Alto | Media (draw tracker) | instancing mallas repetidas |
| H5 | **Immediate-mode residual** (`RenderMeshAlternative`) | Medio | Fácil (grep usos) | enrutar a arrays/VBO |
| H6 | **Partículas immediate** | Medio | Media (timer) | VBO dinámico (orphaning) |
| H7 | **State changes redundantes** (bind/enable sin ordenar) | Medio | Media (contador binds) | sort por material |
| H8 | **Texturas sin atlas / dup RAM-VRAM** | Medio | Media (dump tabla) | atlas + dedup hash |
| H9 | **Allocations/frame** en collect | Bajo-Medio | Media (hook alloc) | pools/reuse |
| H10 | **Sistemas ocultos updatean** | Bajo-Medio | Media (gate visible/frustum) | freeze invisibles |
| H11 | **TTL 1500ms** → re-descompresión JPEG | Bajo-Medio | Fácil (cache miss log) | LRU por presupuesto |
| H12 | **Modelos BMD cargados N veces** | Bajo (RAM) | Media (instrumentar Open2) | tabla compartida (handle) |

Top 3: **H1 terreno VBO, H2 GPU skinning total, H3 frame pacing.**

---

## 4. Profiling e instrumentación (medir antes de tocar)

| Métrica | Cómo | Dónde |
|---|---|---|
| CPU por subsistema | `ScopedCpuTimer` (QPC) por pass | wrap `RenderXxx` en `RenderGameWorld` |
| GPU time por pass | `glGenQueries`+`GL_TIME_ELAPSED`, leer con lag 2-3 frames | mismo punto |
| Draw calls/pass | contador en wrapper `glDrawArrays`/`glBegin` | wrapper draw |
| State changes/frame | contador en `BindTexture`/`glEnable`/`glBlendFunc` | `ZzzOpenglUtil` |
| Vértices/tris por pass | acumular `NumTriangles*3` | `RenderMesh` |
| Uploads/bytes/frame | contar bytes en pointers/draw y futuro `glBufferData` | wrapper |
| #chars/objetos/partículas/widgets | contadores entidad | cada pass |
| RAM tex/modelos/UI | sumar `W*H*Comp` de `BITMAP_t` + tamaños BMD | audit on-demand |
| VRAM estimada | texturas + VBOs | dump |
| Allocations/frame | override new / arena counter | build debug |
| Cache hit/miss tex | contadores `CBitmapCache` | `GlobalBitmap.cpp` |

Entregables: **perf overlay** (toggle tecla, reusa text-cache), **pass profiler** (`PassStats[]` por enum), **draw-call tracker**, **memory/texture audit dump** (CSV), **benchmark scenes** (extender harness `MU_TEST_CHARS`/`MU_TEST_SHOT`: `empty/crowd100/crowd100fx/town`, cámara+seed fijos).

```cpp
// PassProfiler.h  (capa: Render::Profiling)
enum class Pass { Terrain, Objects, Characters, Effects, Particles, Items, UI, Count };
struct PassStats { double cpuMs=0, gpuMs=0; int draws=0, tris=0, binds=0; };
extern PassStats g_passStats[(int)Pass::Count];

struct ScopedPass {
    Pass p; double t0; GLuint q;
    ScopedPass(Pass p_) : p(p_) {
        t0 = NowMs();
        if (g_gpuTimers) { glGenQueries(1,&q); glBeginQuery(GL_TIME_ELAPSED, q); }
    }
    ~ScopedPass() {
        g_passStats[(int)p].cpuMs += NowMs() - t0;
        if (g_gpuTimers) glEndQuery(GL_TIME_ELAPSED); // leer N frames después
    }
};
// uso en RenderGameWorld():
{ ScopedPass _(Pass::Terrain);     RenderTerrain(false); }
{ ScopedPass _(Pass::Characters);  RenderCharactersClient(); }
```

---

## 5. Quick wins

| Quick win | Beneficio | Complejidad | Riesgo | Medir |
|---|---|---|---|---|
| Perf overlay + pass profiler | Habilita todo | Baja | Nulo | es la medición |
| Frame cap/pacing | -CPU/calor, jitter, fps honesto | Baja | Bajo | dt entre frames |
| Distance culling items/efectos | -draws crowd | Baja | Bajo (pop-in) | draws A/B |
| Freeze sistemas invisibles | -CPU | Baja-Media | Medio (estado stale) | timer pass |
| TTL/cache fix (subir TTL / LRU) | -picos JPEG decode | Baja | Bajo | cache miss count |
| Sort draws por textura/material | -state changes | Media | Bajo | binds/frame |
| Eliminar path immediate vivo | -CPU per-vertex | Media | Medio | grep + A/B screenshot |
| Mipmaps + filtrado | perf (cache GPU) + visual gratis | Baja | Bajo | GPU ms + screenshot |
| Warm cache assets escena al cargar | -hitching | Media | Bajo | hitches/log |

Orden: overlay → frame cap → culling/freeze → mipmaps → TTL → sort binds.

---

## 6. Modernización renderer (incremental, flagged)

| Etapa | Qué | Nota |
|---|---|---|
| M0 | Contexto GL 3.3 (o compat + ext VBO/VAO/shader); GLEW/glad | no rompe fixed-function existente |
| M1 | **VBO/VAO terreno estático** (chunks subidos 1 vez) | primer gran win |
| M2 | VBO dinámico partículas/efectos (orphaning o persistent map) | datos/frame batcheables |
| M3 | Shaders mínimos: replicar GL_MODULATE + alpha test + RENDER_* (chrome/bright/dark) | mantiene identidad visual |
| M4 | **GPU skinning total**: bones→`uniform mat4[]`/UBO + vertex shader hace Transform() | borra H2 |
| M5 | Instancing objetos repetidos (`glDrawArraysInstanced` + attrib divisor) | extiende patrón chars |
| M6 | UBO matrices view/proj | -uploads redundantes |

UBO sí aporta (matrices, bones ≤ límite). SSBO solo si bones exceden UBO — probablemente innecesario. Persistent mapping solo si churn partículas alto. Cada etapa flag `MU_*` default off → A/B + screenshot idéntico → default on.

---

## 7. Terreno / personajes / efectos

**Terreno** (`ZzzLodTerrain.cpp:3065`): miles tiles/frame, 2 pasadas (base+grass/alpha, confirma foro tuservermu), luz/blend per-vértice. → VBO estático por chunk (16×16), índices compartidos, subir 1 vez; alpha-splatting a fragment shader (elimina 2da pasada CPU); cull por chunk AABB (ya hay `TestPoint2D`); generación VBO de chunks nuevos en worker (submission serial). No tocar LOD existente.

**Personajes** (`ZzzCharacter.cpp:11317`, skinning `ZzzBMD.cpp`): bone/anim CPU residual ~10ms crowd (H2); per-char setup ~3.5ms (payoff decreciente). → GPU skinning total (M4); LOD por distancia (menos bones/anim rate lejos). Meshes ya instanced. Collect ya paralelizado — no más threads (Amdahl). No tocar per-char micro-opt.

**Objetos/efectos** (`ZzzObject.cpp:3274/3492/6407`, `ZzzEffect*`): props repetidos = N draws; partículas immediate; items sin distance cull fuerte. → instancing props (M5); VBO dinámico partículas (M2); distance cull items. Billboard partículas en shader. Build partículas a worker posible. No re-tocar blend/wave (ya GPU).

---

## 8. RAM / VRAM

Detección: dup texturas (hash path/contenido en `BITMAP_t`); texturas gigantes (dump por bytes, flag >2048²); modelos N veces (contador por path en `Open2`); falta atlas (contar texturas <256² sueltas); buffers temporales (revisar `RenderArray*`/collect); fugas (RefCount nunca →0); cache sin descarga (verificar cap VRAM, no solo TTL); picos (log carga durante gameplay).

Reducción sin perder calidad: (1) dedup por handle (`path→recurso`, RefCount real); (2) atlas UI+tiles pequeños; (3) compresión GPU BC1/BC3 (4-6× menos VRAM); (4) mipmaps (`glGenerateMipmap`, también win visual); (5) presupuesto LRU (evict bajo presión, no TTL ciego); (6) stream/descarga mapas no presentes.

---

## 9. Estructuras de datos

| Patrón acceso | Estructura | Por qué |
|---|---|---|
| Iteración caliente/frame (chars, tiles, partículas) | Array contiguo / **SoA** | locality, prefetch, vectorización |
| Lookup id denso (textura por enum) | Array indexado directo (ya `Bitmaps[]`) | O(1) sin hash/colisión |
| Lookup clave dispersa (path→recurso) | **Hash map** | clave no-densa; aquí hash gana |
| Insert/borrado + handles estables | Pool + freelist | sin realloc, handle válido |
| Temporal por-frame | Arena (reset fin frame) | free O(1) masivo |

**Hash perjudica** en el loop caliente de render: iterar `unordered_map` = punteros dispersos = cache miss/elemento. Si recorrés todo cada frame → array. Hash solo para lookup esporádico (cargar por nombre), no para iteración per-frame.

**SoA vs AoS:** render de MU favorece SoA en arrays de vértices transformados (ya separados `VertexTransform/NormalTransform/LightTransform` — bien). AoS para acceso por-entidad completa.

Concreto: recursos → hash `path→handle` + array denso por index (las dos cosas). Entidades render → SoA. Temporales → arena.

---

## 10. UI / overhead no gráfico

| Área | Sospecha | Acción |
|---|---|---|
| UI texto | text-cache hecho (5.5→1.38ms) | mantener; rediseño retained diferido |
| UI manager | 4 sorts + 3 copias/frame `CNewUIManager` | 1 sort estable + 0 copias (índices) |
| UI oculta | update gated por IsEnabled ≠ IsVisible → ocultas updatean | gate por visible real |
| Audio | si mezcla en hilo juego | worker/callback SDL_mixer |
| Lógica secundaria | timers, networking visual, interpolación | tick fijo, no per-frame (regresiones previas: efectos/luz a tick) |
| Sistemas ocultos | partículas fuera frustum tickean | freeze |
| Frame pacing | sin cap → busy loop | §5 |

---

## 11. Calidad visual + performance (sin romper identidad MU)

| Mejora | Costo | Cuándo |
|---|---|---|
| Mipmaps + trilinear/anisotrópico | casi gratis (+perf) | ya, con texturas |
| MSAA 2-4× | barato (GPU ociosa) | pronto |
| Texturas BC + filtrado | gratis (-VRAM) | con audit |
| Animaciones más suaves | barato con GPU skin | con M4 |
| Postproceso liviano (bloom sutil, tonemap leve) | medio (shaders/FBO) | tras M3, opt-in |
| Iluminación per-pixel (paleta MU) | medio | tras M3 |
| Upscale/resolución (4K) | medio | tras VBO/shaders |
| Sombras (shadow maps) | **caro** | **no aún** |
| SSAO/GI/PBR | caro + rompe estética | **no** |

Gratis: mipmaps, filtrado, BC, MSAA. Caras-válidas-luego: postproceso, per-pixel, upscale. No tocar: sombras, SSAO, PBR.

---

## 12. Roadmap por prioridad

```
[QUICK WINS]  (alto impacto, riesgo bajo, mide todo)
  1. Perf overlay + pass profiler + draw/state trackers
  2. Frame cap / pacing
  3. Distance culling items/efectos + freeze invisibles
  4. Mipmaps + filtrado (perf + visual gratis)
  5. Fix TTL/cache + sort draws por material

[REFACTORS INTERMEDIOS]  (alto impacto, riesgo medio)
  6. TERRENO VBO estático por chunk        <- mayor win individual restante
  7. Eliminar path immediate vivo (RenderMeshAlternative)
  8. VBO dinámico partículas/efectos
  9. Texture/memory audit -> dedup handles + atlas + BC

[MODERNIZACIÓN FUERTE]  (detrás de flags)
 10. Shaders mínimos (RENDER_* / GL_MODULATE)
 11. GPU SKINNING TOTAL (borra ~10ms crowd)
 12. Instancing objetos repetidos
 13. UBO matrices/bones

[OPCIONALES]
 14. Postproceso liviano, per-pixel light, MSAA alto, upscale
 15. Rediseño UI retained (proyecto aparte, nivel threading)
 16. (NO aún) sombras, SSAO, PBR
```

---

## 13. Entregables

- [ ] Perf overlay (`Render/Profiling/PerfOverlay.{h,cpp}`, toggle tecla, reusa text-cache)
- [ ] Pass profiler (`PassProfiler.{h,cpp}`, `ScopedPass`, CPU+GPU queries)
- [ ] Draw-call / state tracker (wrappers `ZzzOpenglUtil`)
- [ ] Benchmark scenes (extender harness: `empty/crowd100/crowd100fx/town`)
- [ ] Texture/memory audit (CSV `path,w,h,comp,bytes,refcount` + totales)
- [ ] Terrain VBO path (flag `MU_TERRAINVBO`) — ver `02-terrain-vbo.md`
- [ ] Full GPU skinning path (flag `MU_GPUSKIN`)
- [ ] Batching/instancing layer (extiende `MU_GPU*` a objetos)
- [ ] Renderer abstraction notes (doc dedicado: GL3.3 ctx, VBO/VAO/shader, flags)
- [ ] UI profiling (timer `CNewUIManager`: sorts/copias por frame)
- [ ] Resource cache audit (instrumentar `CBitmapCache` + `Open2` dedup)
- [ ] ADRs: terrain-vbo, gpu-skinning, gl33-context

---

## PR inicial propuesto — instrumentación + Terrain VBO

Elegido **terreno** (chars ya optimizados; mayor win restante; geometría estática = bajo riesgo regresión; "hacer menos"). Dos commits.

### Commit 1 — `feat(perf): pass profiler + perf overlay`
- `src/source/Render/Profiling/PassProfiler.{h,cpp}` — enum `Pass`, `g_passStats`, `ScopedPass`.
- `src/source/Render/Profiling/PerfOverlay.{h,cpp}` — tabla con text-cache; toggle tecla.
- Instrumentar `MainScene.cpp:396 RenderGameWorld()`: envolver cada `RenderXxx` en `ScopedPass`.
- Draw/state counters en wrappers `ZzzOpenglUtil`.
- Flag `MU_PERFOVERLAY` (env, default off).
- Validación: `crowd100` → overlay confirma terreno = mayor consumidor. Captura A/B.

### Commit 2 — `feat(perf): static VBO terrain path behind MU_TERRAINVBO`
- Al cargar mapa: VBO por chunk (16×16 tiles): pos+texcoords+luz estática en 1 buffer, índices compartidos.
- `RenderTerrain()`: si flag → bind VBO + `glDrawElements` por chunk visible (cull AABB con `Frustum::TestAABB`); else legacy intacto.
- Re-subir solo chunks con lighting dinámico cambiado (mayoría estática → 0 uploads/frame).
- Flag default off → validar → default on.
- Validación: overlay terreno ms↓ + draws↓ (miles→#chunks); screenshot pixel-idéntico A/B; FPS crowd↑.

```cpp
// build (load-time, por mapa)
struct TerrainChunk { GLuint vbo, ibo; int indexCount; AABB bounds; };
std::vector<TerrainChunk> g_terrainChunks;

void BuildTerrainVBO() {
    for (chunk in 16x16 grid over map) {
        std::vector<TerrainVtx> v;   // pos, uv, staticLight
        std::vector<GLuint> idx;
        for (tile in chunk) emit 2 tris -> v, idx;
        glGenBuffers(1,&c.vbo); glBindBuffer(GL_ARRAY_BUFFER,c.vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(TerrainVtx), v.data(), GL_STATIC_DRAW);
        glGenBuffers(1,&c.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,c.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*4, idx.data(), GL_STATIC_DRAW);
        c.indexCount = idx.size(); c.bounds = computeAABB(chunk);
        g_terrainChunks.push_back(c);
    }
}

// per-frame
void RenderTerrain(bool edit) {
    if (!g_terrainVBO) { RenderTerrainLegacy(edit); return; }
    ScopedPass _(Pass::Terrain);
    for (auto& c : g_terrainChunks) {
        if (!g_frustum.TestAABB(c.bounds)) continue;   // cull por chunk
        glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
        glVertexPointer(3, GL_FLOAT, sizeof(TerrainVtx), (void*)offsetof(TerrainVtx,pos));
        glTexCoordPointer(2, GL_FLOAT, sizeof(TerrainVtx), (void*)offsetof(TerrainVtx,uv));
        glColorPointer(3, GL_FLOAT, sizeof(TerrainVtx), (void*)offsetof(TerrainVtx,light));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.ibo);
        glDrawElements(GL_TRIANGLES, c.indexCount, GL_UNSIGNED_INT, 0);
        g_passStats[(int)Pass::Terrain].draws++;
    }
}
```

Riesgos: alpha/grass 2da pasada → en v1 mantener legacy para esa capa, VBO solo capa base. Lighting dinámico → color array de chunk re-subido solo si cambió. Ambos contenidos por el flag.
