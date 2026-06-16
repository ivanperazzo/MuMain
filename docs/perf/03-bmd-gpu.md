# P3/BMD — Render de modelos a GPU — Deep-dive + plan (props-first)

> Los datos (run12) mandan: el cuello es el **render de modelos BMD** = pasada `Objects`
> (props del mapa, ~17 ms caminando) + `Characters` (multitud, 80-109 ms). Terreno ~2 ms
> (descartado). Effects ~0. Este es el track real para 1400-2000 fps.

## Datos BMD (`Render/Models/ZzzBMD.{h,cpp}`)

`Models[type]` (un `BMD` por tipo de modelo, COMPARTIDO entre instancias):
- `NumBones`, `Bones[]` — esqueleto + animaciones (`BoneMatrixes[action]`).
- `NumMeshs`, `Meshs[]`; cada `Mesh_t`: `Vertices[]` (cada `Vertex_t` tiene `Node` = índice de
  hueso), `Normals[]`, `TexCoords[]`, `Triangles[]` (índices), `Texture`.
- La malla en **espacio modelo es ESTÁTICA** (no cambia salvo editor). Lo que cambia por frame
  son las **matrices de hueso** (animación/ángulo/posición de la instancia).

## Path actual (CPU, por objeto/personaje, cada frame)

1. `BMD::Animation(BoneMatrix, AnimationFrame, …)` — calcula matrices de hueso (QuaternionSlerp).
   ~N_bones. Barato-ish.
2. `BMD::Transform(BoneMatrix, …)` — **por CADA vértice**: `VectorTransform(v->Position,
   BoneMatrix[v->Node], …)` → escribe al buffer GLOBAL `VertexTransform[mesh][v]` (+ normales +
   lighting). **Caro, O(vértices), CPU.**
3. `BMD::RenderMesh` — **por CADA triángulo** arma arrays CPU `vertices/colors/texCoords` desde
   `VertexTransform` + luz por-vértice, luego `glVertexPointer/glColorPointer/glTexCoordPointer`
   (arrays **client-side**) + `glDrawArrays(GL_TRIANGLES, NumTriangles*3)`. **Sin VBO** ⇒ el
   driver copia el array entero en cada draw. 1 draw por mesh por objeto.

Costo/frame ≈ Σ_instancias (skin CPU de todos los vértices + rebuild del array + upload
client-side + 1 draw/mesh). `VertexTransform` es global compartido ⇒ no se puede cachear entre
instancias; se repuebla antes de cada draw. Esto explica los 17 ms (props) + 80-109 ms (chars).

Modos de render (en `RenderMesh` + variantes `RenderMeshAlternative`/`RenderMeshTranslate`,
estas últimas immediate-mode `glBegin`): `RENDER_TEXTURE`, `RENDER_CHROME/CHROME4/OIL` (mapeo de
env), `RENDER_WAVE` (desplaza vértices por normal·sin), `RENDER_SHADOWMAP`, blend/alpha, skin/hair
hide. Hay que cubrirlos (o diferir los exóticos).

## ⚠️ Prerrequisito BLOQUEANTE: no hay loader de GL moderno

- `opengl32.dll` (Windows) solo exporta **GL 1.1**. `glDrawArrays`/`glVertexPointer` (1.1)
  resuelven directo ⇒ andan. Pero `glGenBuffers`/`glBindBuffer`/`glBufferData` (VBO, GL 1.5+) y
  todo shader (`glCreateShader`/`glUseProgram`, 2.0+) necesitan loader.
- **GLEW está incluido (headers) pero NO linkeado** (no hay `.lib`, no `target_link_libraries`)
  y **`glewInit()` nunca se llama**. La DLL `glew32` se copia pero no se usa. ⇒ llamar
  `glGenBuffers` hoy = puntero NULL = crash.
- El contexto lo crea **SDL** (`SDL_GL_CreateContext`). ⇒ loader natural = **`SDL_GL_GetProcAddress`**
  (SDL3 ya está linkeado; sin dependencia nueva).
- **Buena noticia:** el contexto es COMPATIBILITY sin versión pedida ⇒ en una 3070 Ti el driver
  da **GL 4.6 compat**. Eso permite **mezclar** draws con VBO+shader y el immediate-mode legacy
  en el mismo frame ⇒ **migración incremental por subsistema, sin romper el resto.** (Verificar
  versión real por log con `glGetString(GL_VERSION)`.)

## Arquitectura objetivo (compartida props + chars)

Por **tipo de modelo** (1 vez): subir a VBO la malla en espacio-modelo (posición, normal,
texcoord, **índice de hueso** por vértice) + IBO de triángulos. Por **instancia/frame**: setear
matrices de hueso como **uniforms** (~N_bones, barato) + 1 `glDrawElements`. El **vertex shader**
hace skinning (`pos = BoneMatrix[node] * v`) + lighting; **cero trabajo per-vértice en CPU, cero
rebuild de arrays, cero re-upload.** Rígidos (props sin animación / 1 hueso) = caso trivial del
mismo shader (matriz identidad/única).

## Plan props-first (incremental, con toggle de rollback)

- **P-infra (BLOQUEANTE, primero):** loader de GL moderno vía `SDL_GL_GetProcAddress` (cargar
  glGenBuffers/Bind/BufferData/Delete, glCreateShader/ShaderSource/Compile/GetShaderiv,
  glCreateProgram/Attach/Link/Use, glGetUniformLocation/glUniform*/glUniformMatrix4fv,
  glGetAttribLocation/glVertexAttribPointer/glEnableVertexAttribArray, glActiveTexture, VAO).
  Loggear `GL_VERSION`/`GL_RENDERER` + asserts de no-NULL. **Verificable por log, riesgo nulo**
  (no cambia render). Módulo `Render/GL/GLFunctions.{h,cpp}` + `ShaderProgram` + `GpuBuffer`.
- **P-bmd-gpu (props):** renderer GPU para los modelos BMD de la pasada `Objects`. Empezar por
  los **rígidos/simples** (pocos huesos, modo TEXTURE, sin chrome/wave). VBO por tipo + shader de
  skinning+luz. Toggle de chat `$gpubmd on/off` (default off) para A/B y rollback inmediato.
  Medir `objects_ms` antes/después (la infra de pasada ya está). Coexiste con el path legacy.
- **P-bmd-chars:** extender a personajes (la pasada cara): equipo (múltiples BMD por personaje),
  modos chrome/oil/wave, sombras, blend/alpha, hide skin/hair. Medir `chars_ms`. Es el premio
  gordo (80-109 ms).
- Después: terreno con la misma infra (si alguna vez vale; ~2 ms, baja prioridad).

## Riesgos / rollback

- Lighting: hoy es por-vértice en CPU (`IntensityTransform` = normal·luz × `BodyLight` + luz de
  terreno). Reproducir en shader (normal·luz + uniform de BodyLight) sin cambiar el look. Riesgo
  visual ⇒ A/B con `$gpubmd`.
- Modos chrome/oil/wave/shadow: variantes de shader o uniforms; diferir los raros al principio.
- Coexistencia GL: cuidar estado (matrices fixed-function vs uniforms, client-state arrays) al
  alternar legacy/moderno en el mismo frame; resetear estado al salir del path GPU.
- Todo medido en DEBUG; el FPS absoluto sube en Release (medir Release en algún punto).
- Rollback por fase: toggle off + `git revert`.

## Verificación (logs)

`analyze_perf.py` ya da `[T O C E]` por segmento. P-bmd-gpu = bajar `objects_ms`; P-bmd-chars =
bajar `chars_ms`, a 30/60/144, sin regresión visual (A/B `$gpubmd`). Loggear GL_VERSION en
P-infra.

## Estado

Deep-dive hecho. **Siguiente = P-infra** (loader GL vía SDL_GL_GetProcAddress) — prerrequisito
bloqueante, bajo riesgo, verificable por log. Luego props-first con `$gpubmd`.
