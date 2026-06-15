# P6 — UI text-render cache (+ hidden-update skip + manager cleanup) — plan

> Objetivo: bajar el pass de UI (`uinew ~5.5ms`, el costo #1 del frame, GPU ociosa)
> sin rediseñar el sistema de ventanas. Overhead FIJO → sube empty (~300→~400+fps)
> Y crowded. Refinar, NO reemplazar (alineado con la auditoría `docs/ui/UI_MAP.md`,
> hipótesis H1-H9 / mejoras M1-M11).
>
> Estado: PLAN. Ejecutar en sesión fresca. Datos que lo motivan abajo.

## Evidencia (profiling in-game, commit 2a809846, scene=5 crowded)

`[frame] … uinew=5.5ms` = el pass más caro (chars ~7, objects ~4, terrain ~0.4
post-VBO, sim ~1.3, cloth 0). Per-window tracer `[ui_win]` (CNewUIManager::Render):
MyInventory ~0.9ms, NameWindow ~0.7ms (escala multitud), 3DCamera ~0.7ms, + muchas
ventanas chicas + **el grueso en texto inmediato**. GPU ocioso (swap <1ms) → 100% CPU.

## Smoking guns (file:line, verificados en `src/source/`)

- **H1 — texto sin cache (el grueso del 5.5ms).** `CUIRenderTextOriginal::RenderText`
  (`UI/Legacy/UIControls.cpp:2821`) por CADA string, CADA frame:
  `TextOut` GDI (2927) rasteriza → `WriteText` memcpy (2703) → `UploadText`
  `glTexSubImage2D` (2751 → 2793/2809) sube a textura → quad. Sin cache de
  string/glifos. El HUD redibuja texto idéntico (labels, “HP/MP”, nombres, [SetOption],
  contadores) cada frame.
- **H2 — ventana oculta sigue en Update.** `CNewUIObj::Update` gateado por
  `IsEnabled()`=`m_bUpdate` (`NewUIManager.cpp:189`), NO por `IsVisible()`=`m_bRender`.
  `Show(false)` baja `m_bRender` pero NO `m_bUpdate` → oculta sigue updateando.
- **H3 — CNewUIManager por frame, sin importar visibilidad.** 4× `std::sort`
  (Update L184, Render L203, Mouse L111, Key L147) + 3× copia del vector (L112,L149,
  L204) + mouse O(N²) (`std::find` en loop, L123).

## Enfoque por incremento (ROI: H1 → H2 → H3)

### Increment 1 — H1: cache de texto string→textura  (el gran win) — HECHO ✅

**Implementado + medido in-game (MAIN_SCENE, scene=5):** `uinew 5.5 → 1.38 ms`
(~4x; objetivo cumplido). Hit-rate **98–99 %**, ~590 strings/frame, ~6-10 miss
(texto dinámico), LRU capado en 2048 entradas. Sin crash, texto nítido / colores
OK / sin stale (validado por el usuario en juego: inventario/skills/chat/nombres).
**Default flippeado ON** (kill-switch `MU_UITEXTCACHE=0`).

Implementación real: textura GL propia por string (key = `hash(texto + color +
HFONT)`, layout/clip aplicado al dibujar vía UVs → key independiente de la caja).
HIT salta `TextOut` + conversión RGBA + `glTexSubImage2D` y dibuja el quad con
`RenderBitmap(-glTex)` (el id negativo bindea la textura cruda). MISS rasteriza
como antes, crea la textura (NEAREST/CLAMP, igual que BITMAP_FONT) y la cachea.
Flush del cache al cambiar `g_fScreenRate`. Archivos: `UI/Legacy/UITextCache.{h,cpp}`
(nuevo) + `UI/Legacy/UIControls.{cpp,h}` (`RenderText` + `BuildTextRGBA`).
Pendiente menor: validación multi-mapa (cache es agnóstico al mapa, bajo riesgo).

#### Diseño original


- **Idea:** key = hash(string + font + textColor + bgColor + flags). Cache LRU
  `key → {GLtexture, w, h}`. En `RenderText`: si HIT → saltar TextOut+memcpy+
  glTexSubImage2D, dibujar el quad con la textura cacheada. Si MISS → rasterizar
  como hoy, guardar en cache, dibujar.
- **Invalidación:** la key incluye TODO lo que cambia el pixel (texto/font/colores/
  multiline/sort). Texto dinámico (chat, contadores) → MISS frecuente pero los
  labels estáticos (mayoría) → HIT. Cap del cache (p.ej. 512 entradas) + LRU evict;
  liberar GL textures al evict. Drop del cache en cambio de resolución/font.
- **Dónde:** envolver `CUIRenderTextOriginal::RenderText` (2821) + `UploadText`
  (2751). Mantener una textura por entrada (o atlas — M-futuro). Reusar el path de
  quad existente.
- **Flag:** `MU_UITEXTCACHE` (default OFF hasta validar; luego ON).
- **Riesgos:** texto stale si la key omite un input (auditar TODOS los setters:
  SetFont/SetTextColor/SetBgColor/SetRenderShadow/…); memoria GL (cap+LRU);
  sombras/bordes (incluir en el render cacheado o en la key).
- **A/B:** in-game, comparar HUD (texto nítido, sin stale, colores OK) + leer
  `uinew=` del `[frame]`. Esperado: uinew 5.5→~1-2ms.

### Increment 2 — H2: no updatear ventanas ocultas
- Gatear `CNewUIObj::Update` también por `IsVisible()` (o por un flag combinado),
  en `CNewUIManager::Update` (`NewUIManager.cpp:~184-189`).
- **Riesgo:** ventanas que necesitan update de fondo (timers de evento, cooldowns,
  buffs que cuentan ocultos). Auditar cuáles y excluirlas (whitelist) o usar
  `Enable(false)` selectivo. Validar que nada deje de actualizarse mal.

### Increment 3 — H3: limpiar CNewUIManager
- Ordenar `m_vecUI` solo cuando cambia (dirty flag en add/remove/cambio de layer),
  cachear el vector ordenado; eliminar las 3 copias `auto x = m_vecUI` por frame
  (Update/Render/Mouse/Key); mouse find O(N²) → mapa/orden.
- Bajo riesgo, ganancia chica (~0.2-0.5ms) pero limpia.

## Profiling a agregar
- Pass `U` en `FrameProfiler` (ya hay UILegacy/UINew slots de la sesión previa;
  reusar). Contador hit/miss del cache de texto (log cada 30 frames) para medir
  hit-rate y dimensionar el cache.

## Verificación / done
- Cada increment detrás de su flag, A/B in-game (login + 1 mapa normal + abrir
  inventario/skills/chat, que es donde más texto hay). Sin regresión visual de
  texto. `uinew` medido antes/después por increment. Flip default ON al validar.
- Probar resolución distinta + cambio de idioma (texto cambia) para la invalidación.

## Después de P6
Restante hacia >500 crowded = **threading (Fase B)** (sacar sim/red/IO del hilo
render) — su propio proyecto, mayor riesgo. Alternativa estratégica: cerrar el track
perf (ganancia acumulada 5→~50fps crowded, render 60→11ms) y volver al objetivo
real del proyecto = **server-authority** (CLAUDE.md). Ver [[ui-window-system-redesign-future]]
para el rediseño retained-mode completo (si alguna vez se quiere el salto grande).
