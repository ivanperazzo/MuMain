# Etapa 5 — Cámara cinemática (cat E → dt real)  ·  USER-GATE

> **Hallazgo de la investigación (antes de tocar código): Stage 5 es de BAJO VALOR ahora.**
> La cámara cinemática NO está rota por el desacople. Detalle abajo. Decisión de dirección
> pendiente (Gate A).

## Qué se investigó

Sitios de `FPS_ANIMATION_FACTOR` en el subsistema de cámara y **en qué escena corren**:

| Sitio | Qué hace | Escena | ¿Acoplado tras 1b? |
|---|---|---|---|
| `CameraMove.cpp:410` `UpdateWayPoint` (camera-walk) | avance de waypoints + delay | **LOG_IN_SCENE** (solo `LoginScene.cpp`) | NO — factor = clamp (1b solo pinea MAIN_SCENE) |
| `CameraMove.cpp:733/734/779/861` `UpdateTourWayPoint` (tour) | accel/speed/delay del tour | **LOG_IN_SCENE** (`DefaultCamera:579` gateado `IsTourMode`; arranca en `LoginScene:308`) | NO — factor = clamp |
| `DefaultCamera.cpp:556` | lerp exponencial del mount-offset (altura de cámara al montar) | **MAIN_SCENE** | **SÍ** (menor) — factor=1.0 ⇒ asienta más rápido a alto FPS |
| `DefaultCamera.cpp:818` | velocity de movimiento de cámara | **EDITOR** (`#ifdef ENABLE_EDIT2`) | irrelevante (no en build normal) |
| `FreeFlyCamera.cpp:202` | speed de free-cam | debug/free-cam | irrelevante (no gameplay) |

### Conclusiones

1. **La cámara cinemática (tour + camera-walk) corre SOLO en LOG_IN_SCENE.** Ahí
   `FPS_ANIMATION_FACTOR` sigue siendo el clamp `REFERENCE_FPS/FPS` (1b solo lo fijó a 1.0
   en MAIN_SCENE). ⇒ **sigue compensada por FPS**; el desacople NO la rompió. Además
   LOG_IN_SCENE corre con VSync (capeado a 60), no es el path de "1400-2000 fps".
2. El único acoplamiento real en MAIN_SCENE es `DefaultCamera:556` (suavizado de altura de
   cámara al montar). Es **menor** (cosmético, transitorio al montar).
3. El follow de cámara in-game NO usa factor: lee `Hero->Object.Position`, que ya está
   interpolada (Stage 2) ⇒ cámara suave a cualquier FPS.

⇒ **Stage 5 como estaba planeado (portar la cinemática a dt) no arregla ningún bug actual.**
Su único valor es **preparación para Stage 8** (cuando se elimine `FPS_ANIMATION_FACTOR`,
la cámara de login y `DefaultCamera:556` perderían su compensación y habría que portarlas
a dt real). Es decir: Stage 5 conviene hacerlo **junto con / justo antes de** Stage 8, no
como paso independiente urgente.

## Opciones (Gate A — decisión del usuario)

- **A — Stage 5 mínimo ahora:** portar `DefaultCamera:556` (mount-offset) a dt real y la
  cinemática de login a dt real, como prep de Stage 8. Bajo riesgo, bajo valor inmediato.
  Difícil de verificar empíricamente (login capeado a 60 FPS).
- **B — Saltar a Stage 6 (efectos/física):** ESTOS sí corren en MAIN_SCENE y SÍ están
  acoplados tras 1b (partículas, `PhysicsManager.Move(0.025*factor)`, decays). Mayor
  impacto en el "feel" a alto FPS. Ver `KNOWN-ISSUES.md` #3.
- **C — Pivotar al objetivo GPU/alto-FPS:** el núcleo del desacople (1b–4b) ya está: el
  juego corre a velocidad correcta y se ve suave a cualquier FPS. Lo que queda
  (cámara/efectos) es pulido. El objetivo real ("entrar y tener 1400-2000 fps con la 3070
  Ti") es la pista de optimización gráfica/GPU — track separado.

## Si se hace (diseño para A, futuro)

Mismo patrón que 4a: helper puro `CameraTiming::Step(base, frameMs)` (= `base·frameMs/40`,
clamp) + doctest de invariancia FPS; aplicar en `DefaultCamera:556` (dt ya está como
`factor/25`, cambiar a `frameMs/40`) y en `UpdateTourWayPoint`/`UpdateWayPoint`. Verificación
por log del recorrido de cámara (duración total constante a 30/60/144).
