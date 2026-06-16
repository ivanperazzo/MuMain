#!/usr/bin/env python3
# Stage 6a (verificacion IN-GAME): a diferencia de analyze_effect.py (que
# RECONSTRUYE el step desde frame_ms), este lee las columnas eff_step/eff_decay
# que el cliente escribe muestreando el GLUE REAL (Render::EffectTiming::
# EffectStep()/EffectDecayExp(0.8)) una vez por frame de render dentro de
# MAIN_SCENE. Prueba que la sustitucion a dt ocurre en RUNTIME, no solo en el
# test puro.
#
# Espera el esquema 14-col:
#   t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,hero_units_per_sec,steps,
#   interp_alpha,frame_ms,hero_anim,hero_anim_render,eff_step,eff_decay
#
# Lo correcto (fix 6a):
#   sum(eff_step)/seg == 25 a CUALQUIER FPS (un lifetime `-= eff_step` decae 25/s).
#   eff_decay^(frames/seg) == 0.8^25 a cualquier FPS.
#   ademas eff_step == clamp(frame_ms,250)/40 (el glue resolvio a dt, no a 1.0).
import sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run09_s6a.csv"
REF_TICK_MS = 40.0
MAX_FRAME_MS = 250.0
DECAY_BASE = 0.8

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 14:
        continue
    try:
        rows.append((float(p[0]), float(p[1]), float(p[9]),
                     float(p[12]), float(p[13])))  # t_ms, fps, frame_ms, eff_step, eff_decay
    except ValueError:
        pass

if not rows:
    print(f"sin filas validas en {path} (esquema 14-col con eff_step/eff_decay?)"); sys.exit(1)

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

bt = defaultdict(float)
lin_sum = defaultdict(float)        # sum(eff_step) -> debe dar 25/s
exp_prod = defaultdict(lambda: 1.0) # product(eff_decay)
frames = defaultdict(int)
match_err = defaultdict(float)      # |eff_step - clamp(frame_ms)/40| acumulado
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    if dt <= 0 or dt > 1.0:
        continue
    fms, step, decay = rows[i][2], rows[i][3], rows[i][4]
    if fms <= 0:
        continue
    b = bucket(rows[i][1])
    bt[b] += dt
    frames[b] += 1
    lin_sum[b] += step
    exp_prod[b] *= decay
    match_err[b] += abs(step - min(fms, MAX_FRAME_MS) / REF_TICK_MS)

print(f"=== Stage 6a IN-GAME: glue real (eff_step/eff_decay)  ({path}) ===")
print(f"{'bin':<7}{'segs':>8}{'frames':>8}{'lineal/s':>12}{'exp/s':>12}{'eff_step~dt':>14}")
lin_rates = {}
for b in ["~30", "~60", "~144", ">200"]:
    if bt[b] < 0.5:
        continue
    lin = lin_sum[b] / bt[b]
    exp = exp_prod[b] ** (1.0 / bt[b])
    avg_err = match_err[b] / max(frames[b], 1)
    lin_rates[b] = lin
    print(f"{b:<7}{bt[b]:>8.1f}{frames[b]:>8}{lin:>12.2f}{exp:>12.4f}"
          f"{('OK' if avg_err < 1e-3 else f'{avg_err:.3f}'):>14}")

if lin_rates:
    vals = list(lin_rates.values())
    disp = (max(vals) - min(vals)) / (sum(vals) / len(vals)) * 100
    print(f"\nlineal: esperado 25.0/s a todo FPS; dispersion entre bins = {disp:.1f}%  "
          f"=> {'6a OK in-game' if disp < 5 else 'REVISAR'}")
print(f"exp: esperado 0.8^25 = {DECAY_BASE**25:.4f} a todo FPS")
print("eff_step~dt: 'OK' = el glue resolvio a clamp(frame_ms)/40 (no a 1.0) en MAIN_SCENE")
