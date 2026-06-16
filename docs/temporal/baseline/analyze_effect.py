#!/usr/bin/env python3
# Stage 6a: prueba por log de que decays/lifetimes/timers de efectos son
# FPS-independientes tras el swap `* FPS_ANIMATION_FACTOR` -> `* EffectStep()`
# (y `pow(k, factor)` -> `EffectDecayExp(k)`).
#
# `EffectStep()` en MAIN_SCENE == `clamp(frame_ms, 250) / 40` -- EXACTAMENTE la
# misma cantidad que Stage 4a (AnimTiming::FrameSpeed con base=1). Por eso este
# analizador reusa la columna frame_ms de cualquier CSV ya capturado (run06+):
# no hace falta relanzar. Reconstruye, con los timings REALES de la corrida, dos
# magnitudes de efecto y muestra OLD (1x por render frame, el bug post-1b) vs
# NEW (el fix):
#
#   (a) decay lineal / lifetime: `LifeTime -= 1 * step` por frame.
#       OLD -> ~FPS unidades/seg (efectos expiran ~FPS/25 mas rapido).
#       NEW -> sum(frame_ms)/40 = 25 unidades/seg, PLANO a todo FPS = la sim.
#   (b) decay exponencial: `x *= pow(0.8, step)` por frame.
#       NEW -> 0.8^25 por segundo a cualquier FPS; OLD -> 0.8^FPS (mucho menor).
#
# Esquema esperado (>=10 col): t_ms,fps,...,frame_ms (col 9, 0-based).
import sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run06_s4.csv"
REF_TICK_MS = 40.0    # 25 tps
MAX_FRAME_MS = 250.0  # mismo clamp que EffectTiming/SimulationClock
DECAY_BASE = 0.8      # GOBoid butterfly damping

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
    print(f"sin filas validas en {path} (esquema con frame_ms?)"); sys.exit(1)

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

# por bin FPS: tiempo, lifetime OLD/NEW por seg, y producto de decay exp OLD/NEW.
bt = defaultdict(float)
lin_old = defaultdict(float); lin_new = defaultdict(float)
exp_old = defaultdict(lambda: 1.0); exp_new = defaultdict(lambda: 1.0)
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    if dt <= 0 or dt > 1.0:
        continue
    fms = rows[i][2]
    if fms <= 0:
        continue
    b = bucket(rows[i][1])
    step_new = min(fms, MAX_FRAME_MS) / REF_TICK_MS   # EffectStep() en MAIN_SCENE
    bt[b] += dt
    lin_old[b] += 1.0                                 # `-= 1` por render frame (bug)
    lin_new[b] += step_new                            # el fix
    exp_old[b] *= DECAY_BASE ** 1.0                   # `*= 0.8` por frame (bug)
    exp_new[b] *= DECAY_BASE ** step_new              # EffectDecayExp(0.8)

print(f"=== Stage 6a: decays/lifetimes/timers de efectos  ({path}) ===")
print("(a) decay lineal / lifetime  -- unidades consumidas por segundo:")
print(f"{'bin':<7}{'segs':>8}{'OLD (bug)':>12}{'NEW (fix)':>12}")
new_lin = {}
for b in ["~30", "~60", "~144", ">200"]:
    if bt[b] < 0.5:
        continue
    o = lin_old[b] / bt[b]; n = lin_new[b] / bt[b]
    new_lin[b] = n
    print(f"{b:<7}{bt[b]:>8.1f}{o:>12.1f}{n:>12.1f}")

if new_lin:
    vals = list(new_lin.values())
    disp = (max(vals) - min(vals)) / (sum(vals) / len(vals)) * 100
    print(f"NEW: dispersion lineal entre bins FPS = {disp:.1f}%  "
          f"=> {'6a OK (decay plano ~25/s)' if disp < 5 else 'REVISAR'}")

print(f"\n(b) decay exponencial  -- factor restante tras ~1 s (base {DECAY_BASE}):")
print(f"{'bin':<7}{'segs':>8}{'OLD (bug)':>14}{'NEW (fix)':>14}")
for b in ["~30", "~60", "~144", ">200"]:
    if bt[b] < 0.5:
        continue
    # normalizar el producto a exactamente 1 s para comparar entre bins
    o = exp_old[b] ** (1.0 / bt[b]); n = exp_new[b] ** (1.0 / bt[b])
    print(f"{b:<7}{bt[b]:>8.1f}{o:>14.4f}{n:>14.4f}")
print(f"NEW esperado a todo FPS: {DECAY_BASE}^25 = {DECAY_BASE**25:.4f}")
