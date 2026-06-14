#!/usr/bin/env python3
# Stage 4b: prueba por log de que la POSE del cuerpo se suaviza entre ticks.
# Compara el frame de animacion del Hero crudo (hero_anim, escalona @25Hz) contra
# el interpolado de render (hero_anim_render, avanza su fraccion cada frame).
#
# Esquema 12-col:
#  t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,hero_units_per_sec,steps,
#  interp_alpha,frame_ms,hero_anim,hero_anim_render
#    0    1     2      3        4            5               6            7
#    8        9         10          11
#
# Pruebas:
#  (A) %frames con cambio en hero_anim_render  >>  %frames con cambio en hero_anim
#      (render avanza casi cada frame; crudo solo en frames con tick).
#  (B) deteccion $interp off: hero_anim_render == hero_anim exacto.
import sys, math
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run07_s4b.csv"
EPS = 1e-4

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 12:
        continue
    try:
        rows.append((float(p[0]), float(p[1]), int(float(p[7])),
                     float(p[10]), float(p[11])))   # t, fps, steps, anim, anim_render
    except ValueError:
        pass

if not rows:
    print(f"sin filas validas en {path} (esquema 12-col con hero_anim?)"); sys.exit(1)

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

def frac(x):
    return x - math.floor(x)

# solo frames donde la animacion del cuerpo esta avanzando (crudo cambia en +-6 frames)
def advancing(i):
    w = rows[max(0, i-6):i+6]
    for k in range(1, len(w)):
        if abs(w[k][3] - w[k-1][3]) > EPS:
            return True
    return False

raw_chg = defaultdict(int); ren_chg = defaultdict(int); nf = defaultdict(int)
eq = 0; tot = 0
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    if dt <= 0 or dt > 1.0:
        continue
    tot += 1
    if abs(rows[i][4] - rows[i][3]) < EPS:   # render == raw exacto
        eq += 1
    if not advancing(i):
        continue
    b = bucket(rows[i][1])
    nf[b] += 1
    if abs(frac(rows[i][3]) - frac(rows[i-1][3])) > EPS:   # raw frac cambio
        raw_chg[b] += 1
    if abs(frac(rows[i][4]) - frac(rows[i-1][4])) > EPS:   # render frac cambio
        ren_chg[b] += 1

print(f"=== Stage 4b: suavidad de pose del cuerpo  ({path}) ===")
pct_eq = 100.0*eq/tot if tot else 0
print(f"frames con render==raw exacto: {eq}/{tot} ({pct_eq:.1f}%)  "
      f"=> {'INTERP OFF (o quieto)' if pct_eq > 80 else 'interp activo en parte/todo'}")

print("\nframes en movimiento de animacion, % con cambio de fraccion por frame:")
print(f"{'bin':<7}{'frames':>8}{'%raw-chg':>10}{'%ren-chg':>10}")
for b in ["~30", "~60", "~144", ">200"]:
    n = nf[b]
    if n < 10:
        continue
    print(f"{b:<7}{n:>8}{100.0*raw_chg[b]/n:>9.1f}%{100.0*ren_chg[b]/n:>9.1f}%")

print("\nLectura: %ren-chg >> %raw-chg => la pose interpolada avanza cada frame")
print("(suave) mientras la cruda escalona a 25 Hz. [PRUEBA A]")
