#!/usr/bin/env python3
# Analiza un CSV Stage 3 (esquema 9-col) para probar la INTERPOLACION de render.
#
# Esquema esperado:
#   t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,hero_units_per_sec,steps,interp_alpha
#
# Idea de la prueba:
#   - hero_x/hero_y  = posicion CRUDA de sim. Avanza a SALTOS, ~25 veces/seg
#     (un salto solo en los frames con steps>0). Entre ticks queda quieta.
#   - hero_render_x/y = posicion INTERPOLADA que realmente se dibuja. Debe
#     moverse en pasos CHICOS y FRECUENTES (casi cada frame) rellenando el hueco
#     entre ticks -> render suave a alto FPS.
#
# Pruebas objetivas (solo sobre tramos en movimiento):
#   (A) %frames con delta-render>0 debe ser >> %frames con delta-raw>0.
#       (raw solo cambia en ~1 de cada N frames; render cambia casi siempre)
#   (B) salto maximo por-frame del render << salto maximo del raw.
#       (interp parte el salto de 1 tick en varios frames)
#   (C) deteccion interp on/off: si render==raw en todo el tramo -> interp OFF.
import sys, math
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run05_s3.csv"
MOVEMIN = 30.0     # u/s, ventana por debajo = quieta (descarta)
TELE = 2500.0      # u/s, por encima = teleport/warp (corta)
EPS = 1e-4         # umbral "se movio" en unidades

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 9:
        continue
    try:
        rows.append({
            't': float(p[0]), 'fps': float(p[1]),
            'rx': float(p[2]), 'ry': float(p[3]),       # raw
            'dx': float(p[4]), 'dy': float(p[5]),       # drawn (render)
            'steps': int(float(p[7])), 'alpha': float(p[8]),
        })
    except ValueError:
        pass

if not rows:
    print(f"sin filas validas en {path} (esquema 9-col?)"); sys.exit(1)

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

# Recorre frame a frame, solo dentro de tramos en movimiento (raw avanzando).
raw_moved = defaultdict(int); ren_moved = defaultdict(int); nframes = defaultdict(int)
raw_jump_max = defaultdict(float); ren_jump_max = defaultdict(float)
render_eq_raw = 0; render_total = 0

# segmenta en ventanas de 0.5s para decidir si "esta en movimiento"
i = 1
while i < len(rows):
    a, b = rows[i-1], rows[i]
    dt = (b['t'] - a['t']) / 1000.0
    if dt <= 0 or dt > 1.0:
        i += 1; continue
    d_raw = math.hypot(b['rx']-a['rx'], b['ry']-a['ry'])
    d_ren = math.hypot(b['dx']-a['dx'], b['dy']-a['dy'])
    # velocidad real (cruda) por ventana corta para decidir movimiento
    # usamos el salto raw acumulado: si en ~10 frames se movio, esta caminando
    spd_raw = d_raw / dt
    if spd_raw > TELE:           # teleport: ignorar este par
        i += 1; continue

    bk = bucket(b['fps'])
    # render==raw exacto? (deteccion interp off)
    render_total += 1
    if abs(b['dx']-b['rx']) < EPS and abs(b['dy']-b['ry']) < EPS:
        render_eq_raw += 1

    # solo contar tramos donde realmente camina: mira ventana hacia adelante
    # (aprox: cuenta el frame si hubo algun salto raw en +-5 frames)
    win = rows[max(0,i-5):min(len(rows),i+5)]
    moving = False
    for k in range(1, len(win)):
        if math.hypot(win[k]['rx']-win[k-1]['rx'], win[k]['ry']-win[k-1]['ry']) > EPS:
            moving = True; break
    if not moving:
        i += 1; continue

    nframes[bk] += 1
    if d_raw > EPS: raw_moved[bk] += 1
    if d_ren > EPS: ren_moved[bk] += 1
    raw_jump_max[bk] = max(raw_jump_max[bk], d_raw)
    ren_jump_max[bk] = max(ren_jump_max[bk], d_ren)
    i += 1

print(f"=== analisis interp: {path} ===")
print(f"frames totales (no-teleport): {render_total}")
pct_eq = 100.0*render_eq_raw/render_total if render_total else 0
print(f"frames con render==raw exacto: {render_eq_raw} ({pct_eq:.1f}%)  "
      f"=> {'INTERP OFF (o quieto)' if pct_eq > 80 else 'interp activo en parte/todo'}")

print("\n=== por bin FPS (solo tramos en movimiento) ===")
print(f"{'bin':<7}{'frames':>8}{'%raw-mov':>10}{'%ren-mov':>10}{'saltoMaxRaw':>13}{'saltoMaxRen':>13}")
for bk in ["~30","~60","~144",">200"]:
    n = nframes[bk]
    if n < 10: continue
    pr = 100.0*raw_moved[bk]/n
    pe = 100.0*ren_moved[bk]/n
    print(f"{bk:<7}{n:>8}{pr:>9.1f}%{pe:>9.1f}%{raw_jump_max[bk]:>13.2f}{ren_jump_max[bk]:>13.2f}")

print("\nLectura:")
print("  - %ren-mov >> %raw-mov  => render rellena entre ticks (suave). [PRUEBA A]")
print("  - saltoMaxRen << saltoMaxRaw => interp parte el salto del tick. [PRUEBA B]")
print("  - a mas FPS, %raw-mov baja (mismos 25 ticks/s en mas frames) pero")
print("    %ren-mov se mantiene alto => mas frames suaves por tick.")
