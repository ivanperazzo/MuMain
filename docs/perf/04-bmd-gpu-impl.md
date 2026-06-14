# P-bmd-gpu — Plan de implementación: render BMD a GPU (props-first)

> Fundación lista y verificada (loader GL 4.6 + ShaderProgram/GpuBuffer + self-test PIPELINE OK).
> Acá el plan del renderer BMD a GPU. Empieza por la pasada **Objects** (props), detrás de
> `$gpubmd on/off`, midiendo `objects_ms`. Luego personajes.

## Principio rector (A/B idéntico salvo el costo)

El shader **replica exactamente** la matemática del path legacy, en el mismo orden y bajo el
mismo modelview fixed-function (que NO se toca). Así no hay que resolver si la posición del
objeto vive en las bone matrices o en el modelview: se reproduce tal cual. La única diferencia es
QUIÉN hace el trabajo (GPU vs CPU per-vértice). Visualmente igual ⇒ A/B con `$gpubmd` valida sin
ambigüedad. Se MANTIENE `BMD::Animation` (calcula `BoneTransform[]`, barato). Se ELIMINA
`BMD::Transform` (el loop CPU per-vértice) y el rebuild de arrays client-side de `RenderMesh`.

## Matemática exacta a replicar (de `BMD::Transform` + `RenderMesh`)

**Posición** (por vértice, `v->Node` = hueso):
```
vp = BoneMatrix[v->Node] * v->Position           // VectorTransform (afín 3x4)
if (Translate) { vp *= BodyScale; vp += BodyOrigin; }   // escala escalar + offset mundo
gl_Position = gl_ModelViewProjectionMatrix * vec4(vp, 1)
```
En GLSL, parametrizado por uniforms `uBodyScale`/`uBodyOrigin` (Translate=false ⇒ 1 / 0, el
modelview trae el translate). (Caso `BoneScale != 1` es raro — fallback a legacy por ahora.)

**Luz** (por normal, `sn->Node` = hueso del normal):
```
tn  = rotate(sn->Normal, BoneMatrix[sn->Node])   // VectorRotate (parte 3x3)
lum = dot(tn, LightPosition) * 0.8 + 0.4
lum = max(lum, 0.2)
color.rgb = BodyLight * lum ;  color.a = alpha   // = LightTransform en RenderMesh
```
`LightPosition` = `VectorIRotate(PositionConst, AngleMatrix(ShadowAngle))` (chico, recomputar por
frame/objeto). `BodyLight` = `b->BodyLight` (por objeto). Caso `StreamMesh` (color = BodyLight
directo, sin luz) ⇒ uniform flag o fallback.

## Layout del VBO (por TIPO de modelo, subido 1 vez)

`RenderMesh` dibuja `glDrawArrays(GL_TRIANGLES, NumTriangles*3)` expandido (no indexado). Espejar:
por cada triángulo, por cada uno de sus 3 vértices, empacar un vértice de VBO:
```
struct GpuVtx {
  float pos[3];      // m->Vertices[VertexIndex[k]].Position
  float vBone;       // m->Vertices[VertexIndex[k]].Node
  float normal[3];   // m->Normals[NormalIndex[k]].Normal
  float nBone;       // m->Normals[NormalIndex[k]].Node
  float uv[2];       // m->TexCoords[TexCoordIndex[k]]
};                   // 10 floats/vértice
```
1 VBO por mesh (o por modelo concatenando meshes). Estático (`GL_STATIC_DRAW`), subido la 1ª vez
que se dibuja ese tipo. Cache en una tabla por `o->Type` / por `BMD*`.

## Uniforms por draw

- `uBones[N]` — bone matrices (3x4 → `mat4x3` o `mat4` padded). N = NumBones (cap, ver abajo).
- `uBodyScale` (float), `uBodyOrigin` (vec3), `uLightPos` (vec3), `uBodyLight` (vec3),
  `uAlpha` (float), `uTex` (sampler2D, unit 0).
- `gl_ModelViewProjectionMatrix` (built-in compat) — se reusa el modelview existente.

Set bone uniforms por objeto = ~N_bones × 12 floats (barato vs skinnear todos los vértices).

## Caps y fallbacks (elegibilidad → si no, path legacy)

Una mesh va por GPU sólo si: `IsLoaded()` && `$gpubmd on` && pasada Objects && `renderFlags`
es `RENDER_TEXTURE` (sin chrome/oil/wave/shadow al principio) && `NumBones <= kBoneCap`
(p.ej. 96; uBones grande = límite de uniforms) && `BoneScale == 1` && no StreamMesh/EnableWave.
Cualquier otra → `RenderMesh` legacy. Así props-first sin romper casos raros; se amplía después.

## Punto de integración

Branch al inicio de `BMD::RenderMesh`: si elegible ⇒ `RenderMeshGpu(...)` y `return`; si no,
sigue legacy. Un flag global `BMD::s_gpuObjectsPass` lo activa SOLO alrededor de la pasada
`Objects` (en MainScene, FRAME_PROFILE(Objects)) ⇒ confina GPU a props sin tocar personajes.
`$gpubmd on/off` (consola, default off) prende/apaga global. Estado GL: el path GPU usa
`glUseProgram` + VAO/VBO + `glVertexAttribPointer`; al terminar cada draw, `glUseProgram(0)` y
restaurar client-state para no contaminar el immediate-mode legacy del mismo frame.

## Sub-pasos (incrementales, build+commit c/u)

1. **Shader de skinning** (`Render/GL/BmdShader`): VS skin+luz (extiende el del self-test) + FS
   textura×color. Compila al primer uso; log a `gl_log.txt`.
2. **Cache de mallas GPU** (`Render/Models/BmdGpuCache`): construye/guarda el VBO por `BMD*`+mesh
   (expandido GpuVtx). Subida perezosa.
3. **`RenderMeshGpu`**: bind shader+VBO, set uniforms (bones/light/body), `glDrawArrays`,
   restaurar estado. Sin elegibilidad aún (probar 1 modelo).
4. **Elegibilidad + branch en `RenderMesh`** + flag `s_gpuObjectsPass` alrededor de Objects +
   `$gpubmd`. 
5. **Medir**: `objects_ms` con `$gpubmd off` vs `on` (run nuevo), + A/B visual.
6. Ampliar: más render flags (chrome/wave), cap de huesos, luego personajes (P-bmd-chars).

## Verificación (logs)

`analyze_perf.py` ⇒ comparar `objects_ms` off vs on a 30/60/144. Objetivo: bajar `objects_ms`
(idealmente ~0 CPU, el costo se va a GPU que está idle). Errores de shader → `gl_log.txt`. A/B
visual con el toggle (el usuario compara; el principio rector dice que debe verse igual).

## Riesgos / rollback

- Mismatch visual de luz ⇒ ajustar la fórmula (está exacta arriba) / `$gpubmd off`.
- Límite de uniforms con muchos huesos ⇒ cap + fallback legacy.
- Contaminación de estado GL legacy/moderno en el mismo frame ⇒ restaurar siempre.
- Rollback: `$gpubmd off` (runtime) + `git revert` por sub-paso.

## Estado

Plan listo. Arranca sub-paso 1 (shader de skinning BMD).
