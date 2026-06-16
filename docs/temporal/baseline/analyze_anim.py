#!/usr/bin/env python3
# Stage 4a: prueba por log de que el avance de animación render-path es
# FPS-independiente. Usa la columna frame_ms (esquema 10-col) para reconstruir,
# con los timings REALES de la corrida, las dos tasas de avance de animación:
#
#   OLD (pre-fix): RenderLinkObject sumaba PlaySpeed UNA VEZ POR RENDER FRAME.
#     Para una parte con PlaySpeed=1, eso es 1 frame de animación por frame de
#     render -> tasa = frames/seg ~= FPS. A 144 FPS las alas baten ~5.8x.
#
#   NEW (fix):     suma PlaySpeed * (frame_ms / 40). Sobre 1 s eso da
#     sum(frame_ms)/40 = 1000/40 = 25 frames/seg, PLANO a todo FPS = la sim.
#
# Esquema esperado:
#   t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,hero_units_per_sec,steps,interp_alpha,frame_ms
import sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run06_s4.csv"
REF_TICK_MS = 40.0   # 25 tps
MAX_FRAME_MS = 250.0 # mismo clamp que AnimTiming/SimulationClock

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 10:
        continue
    try:
        rows.append((float(p[0]), float(p[1]), float(p[9])))  # t_ms, fps, frame_ms
    except ValueError:
        pass

if not rows:
    print(f"sin filas validas en {path} (esquema 10-col con frame_ms?)"); sys.exit(1)

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

# acumula por bin FPS: tiempo, avance OLD (1 por frame), avance NEW (frame_ms/40 clamp)
bt = defaultdict(float); b_old = defaultdict(float); b_new = defaultdict(float); bn = defaultdict(int)
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    if dt <= 0 or dt > 1.0:
        continue
    fms = rows[i][2]
    if fms <= 0:
        continue
    b = bucket(rows[i][1])
    bt[b] += dt
    bn[b] += 1
    b_old[b] += 1.0                                   # PlaySpeed=1 por render frame
    b_new[b] += min(fms, MAX_FRAME_MS) / REF_TICK_MS  # el fix

print(f"=== Stage 4a: avance de animacion render-path  ({path}) ===")
print("Tasa de avance (frames de animacion / seg) para una parte con PlaySpeed=1:")
print(f"{'bin':<7}{'segs':>8}{'frames':>8}{'OLD (bug)':>12}{'NEW (fix)':>12}")
old_rates = {}; new_rates = {}
for b in ["~30", "~60", "~144", ">200"]:
    if bt[b] < 0.5:
        continue
    old_r = b_old[b] / bt[b]
    new_r = b_new[b] / bt[b]
    old_rates[b] = old_r; new_rates[b] = new_r
    print(f"{b:<7}{bt[b]:>8.1f}{bn[b]:>8}{old_r:>12.1f}{new_r:>12.1f}")

def spread(d):
    if len(d) < 2: return 0.0
    v = list(d.values()); return (max(v)-min(v))/(sum(v)/len(v))*100

print()
print(f"OLD: dispersion entre bins FPS = {spread(old_rates):.0f}%  "
      f"(escala con FPS = el bug; ~30/60/144)")
print(f"NEW: dispersion entre bins FPS = {spread(new_rates):.1f}%  "
      f"=> {'4a OK (avance plano ~25/s, FPS-independiente)' if spread(new_rates) <= 8 else 'revisar'}")
