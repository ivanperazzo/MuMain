# P-bmd-instance — Render de personajes por instancing (crowd 60fps)

> Hallazgo (run_diag2): pasada Characters = **88 chars visibles × ~30 meshes/char = ~2640
> draws/frame**, single-thread, modo inmediato + skinning CPU. **Draw-call-bound por volumen.**
> La GPU (3080) está ociosa — geometría 2005 es trivial. El cuello es 100% submission CPU.
> El path GPU per-mesh (P-bmd-gpu) NO ayuda en crowd porque no reduce el número de draws.

## Objetivo

Colapsar los ~2640 draws per-mesh en **pocos draws instanciados**, agrupando meshes
idénticas `(BMD*, meshIndex, textura, claseRenderFlags)` entre TODOS los personajes; cada
instancia lleva su propio esqueleto. La GPU skinnea + dibuja; la CPU solo arma buckets.

## Enabler clave (lo que hace viable instancing)

Los personajes se renderizan con **`Translate=true`** ⇒ `BMD::Transform` hornea
`BodyScale`+`BodyOrigin` (= posición mundo del char) en los vértices. El **modelview NO tiene
translate per-char** (verificado: el único `glTranslate` en el path de chars es `RenderGuild`,
el logo, no el cuerpo). Entonces:
- **modelview = vista de cámara, COMPARTIDA por todos los chars del frame** ⇒ se captura 1 vez.
- posición mundo por char = `BodyOrigin` (uniform/atributo por instancia).
- rotación por char = dentro de las bone matrices (Animation usa `Object->Angle`).
- escala por char = `BodyScale`.

⇒ un solo `glDrawArraysInstanced` puede dibujar N chars: `vp = (bone·pos)*scale + origin;
gl_Position = uViewProj * vec4(vp,1)`, con `uViewProj` compartido y `{paletteBase, scale,
origin, color, lit}` por instancia + las bone matrices por char en un TBO.

## Arquitectura (2 fases por frame, pasada Characters)

### Fase COLLECT (durante RenderCharactersClient, reemplaza el draw inmediato de meshes elegibles)
- Por char (una vez): `BMD::Animation` ya calcula su bone palette (NumBones × 3x4). Copiar la
  palette a un buffer CPU per-frame; guardar `paletteBase` (offset en matrices).
- Por mesh elegible: en vez de dibujar, push de `InstanceRec { paletteBase, bodyScale,
  bodyOrigin[3], color[4], lit }` al bucket keyed por `(BMD*, meshIndex, texId, flagClass)`.
  (Geometría base del mesh = el VBO de `BmdGpuCache`, ya existe.)

### Fase FLUSH (después de la pasada)
- Subir el buffer de palettes a un **TBO** (`samplerBuffer`, mat4x3 = 3 texels RGBA32F/bone).
- Capturar `uViewProj` (proj × modelview actuales) una vez.
- Por bucket:
  - Bind textura + estado blend según `flagClass`.
  - Subir el array de `InstanceRec` como **atributos instanciados** (`glVertexAttribDivisor=1`).
  - `glDrawArraysInstanced(GL_TRIANGLES, 0, vertexCount, numInstancias)`.
- VS instanciado: `bone = matFromTBO(paletteBase + aVBone)`; `vp = (bone·aPos)*aScale +
  aOrigin`; `gl_Position = uViewProj * vec4(vp,1)`; luz: `lit?` per-normal `: flat aColor`.

Mismos chars con mismo item ⇒ 1 draw para los N. 88 chars con ~10 items comunes ⇒ ~cientos de
draws en vez de 2640. La ganancia escala con cuánta repetición de equipo haya en pantalla.

## Infra nueva (sub-pasos)

1. **GLLoader += instancing**: `DrawArraysInstanced`, `VertexAttribDivisor`, `TexBuffer`,
   `GenTextures/BindTexture` (1.1 ya), `TexBufferRange` opc. + verificar GL≥3.1.
2. **BonePaletteTBO** (`Render/GL/`): buffer + textura `GL_RGBA32F`; `Begin()` limpia,
   `AppendPalette(mat3x4*, n) -> base`, `Upload()` sube, bind a una unit.
3. **InstancedBmdShader**: VS skin desde TBO + per-instance attribs + `uViewProj`; FS textura×color.
4. **Buckets** (`Render/Models/BmdInstanceBatch`): `Begin()`, `Add(BMD*,mesh,tex,flags,rec)`,
   `Flush()` (subir attrs + instanced draw por bucket). Reusa `BmdGpuCache` para geometría base.
5. **Integración**: en la pasada Characters, si `$gpuinst on`, COLLECT en vez de draw; FLUSH al
   final. Elegibilidad como P-bmd-gpu (RENDER_TEXTURE, sin wave/shadow, BoneScale 1, scale 0,
   bones<cap). No-elegibles (chrome/blend) → path legacy o GPU per-mesh por ahora.
6. **Medir** crowd: draws/frame y chars_ms con `$gpuinst off/on`. Objetivo: draws 2640→cientos,
   chars_ms ↓ fuerte. A/B visual.

## Caps / fallbacks
- bones por char ≤ cap (TBO indexado; cap alto, p.ej. 64). Modelos S6 tienen pocos huesos (foro).
- chrome/wave/blend/escala/StreamMesh ⇒ legacy (no instanciado al inicio).
- Sombra (`RenderBodyShadow`) es pasada aparte (proyección CPU) — fuera de alcance inicial;
  evaluar después (puede ser otro chunk grande de draws).

## Riesgos
- Modelview NO compartido en algún caso especial ⇒ romper. Mitigar: solo instanciar cuando
  `Translate==true` (s_lastTransformTranslate) — si false, legacy.
- TBO/atributos instanciados = GL 3.1+; tenemos 4.6. Errores → `gl_log.txt`.
- A/B visual: debe verse idéntico (misma matemática que el path GPU per-mesh, ya validado).
- Build DEBUG infla todo; medir también Release para el techo real.

## Estado
Plan listo. Modelview-compartido verificado. Arranca sub-paso 1 (GLLoader instancing).
