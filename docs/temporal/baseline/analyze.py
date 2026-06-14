#!/usr/bin/env python3
# Analiza un CSV de TemporalCsvLogger para verificar el desacople temporal (E1).
#
# IMPORTANTE: el movimiento post-1b es a SALTOS DISCRETOS por tick (25/seg). La
# "velocidad por-frame" (dist/dt de un frame suelto) es enganosa: un frame corto
# de alto FPS que justo tuvo 1 tick muestra dist=vel/dt_chico = velocidad inflada.
# Por eso se mide de dos formas correctas:
#   (1) ticks/seg por bin de FPS  -> debe ser ~25 a todo FPS (prueba directa del
#       fixed-step; si fuera per-frame seria ~30/60/144).
#   (2) velocidad real por VENTANAS de tiempo (dist total / tiempo total) ->
#       promedia los saltos; debe quedar plana a todo FPS.
import sys, math, statistics as st
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "run01.csv"
WIN = 0.5         # s, ventana de promediado de velocidad
TELE = 2500.0     # u/s, por encima = teleport/warp (corta ventana)
MOVEMIN = 30.0    # u/s, ventana por debajo = quieta (se descarta)

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 7:
        continue
    try:
        rows.append([float(p[0]), float(p[1]), float(p[2]), float(p[3]), int(float(p[5]))])  # t,fps,x,y,steps
    except ValueError:
        pass

def bucket(f):
    return "~30" if f < 45 else "~60" if f < 90 else "~144" if f < 200 else ">200"

# (1) ticks/seg por bin de FPS
bt = defaultdict(float); bs = defaultdict(int)
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    if 0 < dt < 1.0:
        b = bucket(rows[i][1]); bt[b] += dt; bs[b] += rows[i][4]
print("=== (1) ticks/seg por bin FPS  (objetivo ~25, plano; per-frame seria 30/60/144) ===")
for b in ["~30", "~60", "~144", ">200"]:
    if bt[b] > 0.5:
        print(f"  {b:<7} {bs[b]/bt[b]:6.1f} tps   ({bt[b]:6.1f}s, {bs[b]} ticks)")

# (2) velocidad real por ventanas
wins = []; cur = {'d': 0.0, 'dt': 0.0, 'fps': []}
for i in range(1, len(rows)):
    dt = (rows[i][0] - rows[i-1][0]) / 1000.0
    d = math.hypot(rows[i][2]-rows[i-1][2], rows[i][3]-rows[i-1][3])
    if dt <= 0 or dt > 1.0 or (d/dt if dt > 0 else 0) > TELE:   # corta en stall/teleport
        if cur['dt'] > 0.2: wins.append(cur)
        cur = {'d': 0.0, 'dt': 0.0, 'fps': []}; continue
    cur['d'] += d; cur['dt'] += dt; cur['fps'].append(rows[i][1])
    if cur['dt'] >= WIN:
        wins.append(cur); cur = {'d': 0.0, 'dt': 0.0, 'fps': []}
if cur['dt'] > 0.2: wins.append(cur)

bv = defaultdict(list)
for w in wins:
    spd = w['d'] / w['dt']
    if spd < MOVEMIN: continue
    bv[bucket(sum(w['fps'])/len(w['fps']))].append(spd)

print("\n=== (2) velocidad real (ventana 0.5s) por bin FPS  (objetivo: plano) ===")
means = {}
for b in ["~30", "~60", "~144", ">200"]:
    if bv[b]:
        means[b] = st.mean(bv[b])
        print(f"  {b:<7} {means[b]:7.1f} u/s   (n={len(bv[b])}, mediana={st.median(bv[b]):.1f})")
if len(means) >= 2:
    v = list(means.values()); sp = (max(v)-min(v))/(sum(v)/len(v))*100
    print(f"\n  Dispersion velocidad entre bins FPS: {sp:.1f}%  => "
          f"{'E1 OK (velocidad FPS-independiente)' if sp <= 8 else 'revisar: varia con FPS'}")
