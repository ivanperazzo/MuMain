# P7 — Char pass (Etapa 1.3): medición del collect-walk + veredicto

> Etapa 1.3 del plan maestro era "Char RenderMesh-walk: saltar estado GL
> redundante en el collect". **La hipótesis original quedó descartada por medición**
> (el estado GL ya está cacheado). Este doc registra el breakdown real del char
> pass y qué quedaría por atacar, para no repetir la investigación.

## Medición (harness `MU_TEST_CHARS=100`, login scene, Release, GPU flags ON)

`[frame]` + `[bmd_gpu]` + `[bmd_cov]`:

| señal | valor |
|---|---|
| cpuRender | ~17 ms |
| **chars** | **~11 ms** |
| ├ flush (InstFlush + ShadowFlush, draws GPU) | ~1.1 ms |
| ├ anim (Animation/bone build) | ~1.4 ms |
| └ **collect-walk = chars − flush − anim** | **~8.5 ms** |
| mesh draws/frame | 2300 (23/char) |
| vía GPU | 2100 (91%) → **21 instanced draws** |
| legacy | 200 (blend=100 + geom=100, = 2/char) |
| shadows | 1400 instances → 14 draws |

8.5 ms / 2300 meshes ≈ **3.7 µs/mesh** de puro CPU en el walk (el skinning ya está
diferido por `MU_GPUSKIN`, ver abajo; no es skinning).

## Qué se descartó (medido / leído)

- **GL-state redundante (hipótesis del spec): NO es el costo.** `BindTexture`,
  `EnableAlphaBlend/Test`, `DisableTexture`, etc. (`ZzzOpenglUtil.cpp`) **ya cachean**
  su estado y hacen early-return si no cambia. Saltarlos en el collect rinde ~0.
- **Skinning CPU: ya diferido.** `deferActive` (`ZzzBMD.cpp:343`, gate `MU_GPUSKIN +
  char pass + GPUBMD + GPUINST + GPUSHADOW`) hace que `Transform` saltee los loops
  de skin por-vértice; los 200 meshes legacy se skinnean lazy (`EnsureMeshSkinned`).
  (`skinskip=0` en `[bmd_gpu]` es solo el flag de MEDICIÓN `MU_SKINSKIP`, no el de
  producción — confunde pero no es bug.)
- **`InstPaletteBaseForCurrentPart`**: memoizado por `s_transformSerial` (1×/parte de
  char, no por mesh). Barato.
- **`GetOrBuildMeshGpu`**: 1 lookup `unordered_map<BMD*,...>` por mesh. ~decenas de ns.
- **`InstAdd`**: 1 lookup + 10 `push_back` sin realloc (los vectores persisten). Barato.

⇒ Los sospechosos obvios son baratos. El 8.5 ms es **difuso**: overhead per-char en
`RenderCharactersClient` (equipo, luz, setup × 100) + overhead per-mesh en `RenderMesh`
(branch de `finalRenderFlags`, lookups de textura, bookkeeping × 2300). Sin un
sub-profiling más fino no hay una sola palanca clara.

## Palancas que quedarían (no triviales, payoff incierto)

1. **200 meshes legacy (2/char) → immediate-mode draw + lazy skin.** `blend=100`
   (alpha-blend, p.ej. membrana de alas — el flush es alpha-test/aditivo, no instancia)
   + `geom=100` (1/char, no instanció; no loggea "ineligible" → no es quad/bone, sino
   el gate de no-instanced, p.ej. `s_lastTransformTranslate` falso para esa parte).
   Inmediate-mode (glBegin/glVertex por vértice) de ~200 meshes es caro; moverlos a
   per-mesh GPU o instanced sería un win, pero requiere entender por qué cada uno cae.
2. **Reducir overhead per-mesh del walk** (3.7 µs→<1 µs): aplanar el branch de
   `RenderMesh` / cachear decisiones por (model,mesh). Refactor delicado del hot-path.
3. **Sub-profiling**: separar per-char (RenderCharactersClient body) vs mesh-walk vs
   los 200 legacy, para saber dónde está realmente el 8.5 ms antes de tocar nada.

## Sub-profiling del collect-walk (8.5 ms) — `[rm_prof]` (instrumentación temporal, ya removida)

Split del walk (harness 100ch):

| parte | meshes | tiempo |
|---|---|---|
| **legacy tail** (CPU-skin + draw inmediato) | ~200 (2/char) | **~3.9 ms** ← mayor |
| collect instanced (InstAdd) | ~2100 | ~2.5 ms |
| per-char setup (RenderCharactersClient − mesh-walk) | 100 ch | ~3.5 ms |

Los ~200 legacy NO eran "ineligible geometry": son **blend meshes translúcidos**
(`meshAlphaBlended`: glows de ítem excellent / membranas de ala) que el fix de alas
(`d26ec0e3`) excluía de TODO el bloque GPU → caían a legacy CPU-skin + immediate.
1 mesh distinto (`mesh#1`, glow) compartido por todos los chars → ~1/char.

## Fix implementado — blend meshes por per-mesh GPU (flag `MU_GPUBLENDMESH`)

`RenderMeshGpu` honra el blend state actual + color plano y resetea program/buffer al
salir, así que un blend mesh puede dibujarse **GPU-skinned, alpha-blended, en orden**
(per-mesh, antes del InstFlush, igual que el legacy) — solo el skinning se va a la GPU.
Cambios (`ZzzBMD.cpp`): el bloque GPU acepta `meshAlphaBlended` cuando el flag está ON;
se excluye del `InstAdd` (batch opaco) → cae a `RenderMeshGpu`. Wave meshes siguen
legacy (`!EnableWave` gate), así que los glows con scroll de textura no cambian.

**A/B (harness 100ch):** geom legacy 93→**0**, permeshGPU 2→**191**, GPU 91%→98%,
per-mesh RenderMesh **2.72→2.07 µs (−24 %)**, legacy 91 ms→17 ms por ventana de
138k calls. Screenshots `shot_blend_off/on.jpg`: glows azul/cyan + naranja **idénticos**,
translucidez preservada, sin crash. Win ~1–1.5 ms del char pass (a 100ch).

**Estado:** flag default OFF (regla multi-mapa CLAUDE.md; blend meshes son sensibles —
ver `d26ec0e3`). Activado en `run-temporal-rel.bat` para validación in-game multi-mapa
(Lorencia/Devias/Atlans/especiales + alas/ítems excellent). Flip default ON tras validar.

## Veredicto

1.3 como estaba especificado (skip GL-state) **no procede** (estado ya cacheado). El
win real fue mover los blend meshes translúcidos a GPU (~1–1.5 ms, flag-gated). Lo que
queda (per-char setup ~3.5 ms difuso, collect instanced ~2.5 ms ya eficiente) tiene
payoff decreciente — **decisión estratégica del usuario** (vs threading Etapa 3, vs
volver a server-authority que es la prioridad real del proyecto, CLAUDE.md).

---

## Deep-dive 2 (sesión 14-jun pm) — desglose preciso de RenderMesh

Sub-profiling temporal (`[rm_prof]`/`[char_prof]`/`[rm_diag]`, **ya removido**, árbol limpio).
Harness 100ch, flags producción completos (GPUINST/BMD/SHADOW/SKIN/BLENDMESH).

### Per-char (char_prof)
- `char` (RenderCharacter total) ≈ **10.9 ms/60f**
- `link` (RenderLinkObject, partes equipo) ≈ 4.0 ms (4 calls/char)
- `anim` (FrameProfiler) ≈ 1.4 ms
- **setup = char − RenderMesh − anim ≈ 2.8 ms** (interp/luz/equip/walk) — el "3.5 ms"
  era sobre-estimación; setup real ~2.8 ms y **NO es el mayor lever**.

### RenderMesh (rm_prof) — el grueso del char pass (~7.5 ms/60f, 2300 meshes)
| segmento | ms/60f | meshes | µs/mesh |
|---|---|---|---|
| pre (preámbulo: cascada flags + GL-state setup) | ~0.9 | 2300 | 0.4 |
| inst (InstAdd push) | ~0.27 | 2100 | 0.13 |
| instOverhead (GetOrBuildMeshGpu+rec build+gate) | ~1.2 | 2100 | 0.6 |
| **legacy** (immediate-mode glBegin + force-skin) | **~2.6** | **100** | **26** |
| **permeshGpu** (RenderMeshGpu per-call) | **~2.6** | **100** | **26** |
| getColor (glGetFloatv GL_CURRENT_COLOR) | ~0.02 | 2100 | — (NO es problema warm) |

**Hallazgo central:** los 2100 instanced cuestan 1.5 ms (0.7 µs/mesh); los **200 per-mesh
(cls1+cls3) comen 5.2 ms = 70 %** del RenderMesh a 26 µs/mesh. El path instanced es 17×
más eficiente/mesh. El lever real NO es el setup ni el collect instanced — es **colapsar
esos 200 al batch instanced**.

### Diagnóstico exacto de los 200 (rm_diag, Dark Master set + alas)
- **cls1** (permeshGpu, 1/char, 2.6 ms): `alphaBlend=1` translucent, **eligible=1**, 64 tris,
  flat. Membrana de ala. Excluida de buckets opaque/additive → RenderMeshGpu (overhead
  per-call: `Use()`+`SetBones` uniform upload+attribs+`UseProgram(0)` churn).
- **cls3** (legacy, 1/char, 2.6 ms): textured **BRIGHT + wave=1**, eligible=1, **418 tris**,
  no quads. El gate excluye `EnableWave` (shaders no implementan UV-scroll) → immediate-mode.

### Clave de blend: `EnableAlphaBlend()` = `glBlendFunc(GL_ONE, GL_ONE)` (ADITIVO, depth-write OFF)
(`ZzzOpenglUtil.cpp:327`). Tanto cls1 (wings, no-DARK) como cls3 (BRIGHT) llaman
EnableAlphaBlend ⇒ **son aditivos** ⇒ caben en el **bucket additive EXISTENTE** (`blend=1`,
pass 2 de `InstFlush`). Aditivo = order-independent ⇒ batchearlos al final del pass da
resultado idéntico (riesgo visual mínimo). DARK usa AlphaBlendMinus (subtractivo) → queda
fuera por ahora.

## Plan Etapa 1.4 (próxima sesión) — colapsar per-mesh char meshes al batch instanced

**1.4a — wings translúcidos aditivos → bucket additive instanced** (bajo riesgo, ~2.5 ms):
- Flag nuevo `MU_GPUBLENDINST` (runtime-toggleable, default OFF — panel antilag).
- En el sub-gate de `RenderMesh` (~línea 1546, `ZzzBMD.cpp`): permitir `meshAlphaBlended`
  NON-DARK cuando el flag ON → `InstAdd(mode=0, blend=1)` con `rec.color = BodyLight *
  blendMeshAlpha`, `lit=0`, `instTex=textureIndex`. Quitar/condicionar el `!meshAlphaBlended`.
- Fallback a RenderMeshGpu cuando flag OFF.
- A/B harness: `bmd_cov` permeshGPU 100→~0, inst +100; frame `chars` slot baja; screenshot
  wings idénticos. Luego ping in-game multi-mapa (translucidez de alas).

**1.4b — cls3 textured BRIGHT+wave → additive bucket + wave-UV en shader** (medio, ~2.5 ms):
- Añadir UV-scroll (wave) al InstancedBmdShader (offset UV por `s_instWave`/blendMeshTexCoord,
  réplica exacta de la fórmula legacy). Relajar el gate `!EnableWave` para meshes wave aditivos.
- Más riesgo visual (animación de textura) → A/B + ping in-game.

**1.4c (opcional):** DARK subtractivo → bucket nuevo `blend=3` (GL_ZERO/GL_ONE_MINUS_SRC_COLOR).
Raro; bajo payoff.

**Payoff total estimado:** RenderMesh ~7.5→~2.6 ms ⇒ char pass ~11.5→~6.6 ms ⇒ **~1.7× FPS
en crowded** + offload a GPU (ociosa). Todo flag-gated + runtime-toggleable.

### Reutilizable
`run-harness-full.bat [N] [shotFrame]` — harness Release con TODOS los flags producción +
`MU_FPS`, para medición headless. `bmd_cov`/`bmd_gpu`/`frame` en `gl_log.txt` (Release dir).
`run-harness-ab.bat <blendinst> <waveinst> <tag>` — corre el harness con los flags 1.4a/1.4b,
espera el screenshot, mata el cliente y copia `gl_log_<tag>.txt` + `shot_<tag>.jpg`.
`build-rel.bat` — build Release one-shot (vcvars x86 VS18/Insiders + VS Installer en PATH).

---

## Etapa 1.4 — IMPLEMENTADO + A/B validado (sesión 15-jun)

Ambas sub-etapas: flag-gated, default OFF, runtime-toggleable. Árbol limpio (9 archivos src,
+103/−6, sin instrumentación). Sin GL error, sin crash.

### 1.4a — wings/blend meshes aditivos → bucket instanced additive (`MU_GPUBLENDINST`)
`ZzzBMD::RenderMesh`: un `meshAlphaBlended` NON-DARK (additivo, EnableAlphaBlend=GL_ONE/ONE)
entra al `InstAdd(mode=0, blend=1)` en vez del per-mesh `RenderMeshGpu`. La rama flat `else`
ya capturaba `glColor` (= `BodyLight*blendMeshAlpha`); sólo se fuerza `instBlend=1`. El
fragment `texture*vColor` con `lit=0` reproduce exacto el modulate legacy.

**A/B (harness 100ch Dark Master):**
| flag | inst | permeshGPU | chars ms | cpuRender ms | FPS avg / 1%low |
|---|---|---|---|---|---|
| OFF | 2100 | 100 | ~16.0 | ~24 | 42 / 15 |
| ON  | 2200 | **0** | **~8.7** | **~14** | 47 / 32 |

permeshGPU 100→0, inst +100. **chars −7 ms** (mucho > el ~2.5 estimado: el churn per-call de
`RenderMeshGpu` `Use()`/`SetBones`/`UseProgram(0)` era mayor que el rm_prof "26 µs/mesh").
Screenshots: alas azules translúcidas + glows additivos idénticos.

### 1.4b — textured BRIGHT + UV-scroll (wave) → bucket additive + offset UV en shader (`MU_GPUWAVEINST`)
`EnableWave` (scroll de texcoords `+= BlendMeshTexCoordU/V`, NO el `RENDER_WAVE` de desplaz.
de vértices) ya no excluye del bloque GPU para meshes textured BRIGHT NON-DARK. Rama dedicada
`waveAdditive`: `color=BodyLight`, `lit=enableLight`, `instBlend=1`, `rec.uvScroll` = offset.
El `InstancedBmdShader` aplica `vUV = aUV + uUvScroll` (uniform **por-bucket** — el scroll es
frame-global por (model,mesh): `BlendMeshTexCoordU/V = WorldTime%N * factor`, mismo para todos
los chars del mismo tipo). Añadido `Uniform2fv` al `GLLoader`.

**A/B (harness 100ch, blendinst ON en ambos):**
| flag | inst | legacy blend | chars ms | FPS avg |
|---|---|---|---|---|
| wave OFF | 2200 | 100 | ~10.1 | 39 |
| wave ON  | **2300** | **0** | **~7-9** | **49** |

legacy `blend` 100→0, inst +100. **Ahora el 100 % de los mesh draws de char van por instancing
(inst=2300, todo lo demás 0).** Screenshots: glows de fuego naranja additivos presentes y
correctos.

### Verificación in-game — HECHA (15-jun): "se ve todo perfecto"
Ping in-game (server local + cliente temporal Release, flags ON) confirmó el **scroll animado**
del fuego (1.4b) y la translucidez de alas (1.4a) en movimiento. ⇒ ambos flags flipeados a
**default ON** (`MU_GPUBLENDINST=0` / `MU_GPUWAVEINST=0` desactivan), igual que el precedente
1.3 (`MU_GPUBLENDMESH`).

**Combinado 1.4a+1.4b:** char pass ~16→~7-8 ms, cpuRender ~24→~14 ms, FPS crowd ~42→~49 avg,
1%-low 15→~18-32. Todo el char pass offloaded a la GPU (ociosa). Default ON, commiteado.
