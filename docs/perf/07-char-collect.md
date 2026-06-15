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
