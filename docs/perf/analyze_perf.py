#!/usr/bin/env python3
# P0 (track GPU/alto-FPS): diagnostica CPU-bound vs GPU/present-bound desde el CSV
# con cpu_render_ms / swap_ms (16-col). cpu_render_ms = tiempo de CPU armando/
# enviando el frame (RenderCurrentScene+HUD+ImGui); swap_ms = present
# (SDL_GL_SwapWindow: espera de VSync o catch-up de GPU).
#
# Lectura:
#   - cpu_render_ms ~= frame_ms y swap_ms ~0  -> CPU-BOUND (el cuello es la CPU).
#   - swap_ms grande (con vsync off)          -> GPU/PRESENT-BOUND.
#   - cpu_render_ms * fps ~= 1000             -> CPU saturada.
# Nota: con vsync off el driver puede encolar varios frames, asi que swap_ms puede
# subestimar el costo GPU; el test de tamano de ventana (resize grande->chico, si
# sube el FPS = fill-rate/GPU) lo complementa.
#
# Esquema 16-col: t_ms,fps,hero_x,hero_y,hero_render_x,hero_render_y,
#   hero_units_per_sec,steps,interp_alpha,frame_ms,hero_anim,hero_anim_render,
#   eff_step,eff_decay,cpu_render_ms,swap_ms
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "run10_p0.csv"
SEGMENTS = int(sys.argv[2]) if len(sys.argv) > 2 else 10

rows = []
for ln in open(path, encoding="utf-8", errors="replace").readlines()[1:]:
    p = ln.strip().split(",")
    if len(p) < 16:
        continue
    try:
        rows.append((float(p[0]), float(p[1]), float(p[9]),
                     float(p[14]), float(p[15])))  # t_ms, fps, frame_ms, cpu_render_ms, swap_ms
    except ValueError:
        pass

if not rows:
    print(f"sin filas validas en {path} (esquema 16-col con cpu_render_ms/swap_ms?)"); sys.exit(1)

def summarize(sub, label):
    n = len(sub)
    if n == 0:
        return
    fps  = sum(r[1] for r in sub) / n
    fms  = sum(r[2] for r in sub) / n
    cpu  = sum(r[3] for r in sub) / n
    swap = sum(r[4] for r in sub) / n
    pct_cpu  = 100 * cpu / fms if fms > 0 else 0
    pct_swap = 100 * swap / fms if fms > 0 else 0
    if pct_swap > 50:
        verdict = "GPU/present-bound"
    elif pct_cpu > 60:
        verdict = "CPU-bound"
    else:
        verdict = "mixto/idle-cap"
    print(f"{label:<14}{n:>7}{fps:>9.1f}{fms:>10.2f}{cpu:>11.2f}{swap:>10.2f}"
          f"{pct_cpu:>8.0f}%{pct_swap:>8.0f}%  {verdict}")

print(f"=== P0 perf breakdown  ({path}) ===")
print(f"{'segmento':<14}{'frames':>7}{'fps':>9}{'frame_ms':>10}{'cpu_ms':>11}{'swap_ms':>10}{'%cpu':>9}{'%swap':>8}")
summarize(rows, "TOTAL")
print("-" * 86)

t0, t1 = rows[0][0], rows[-1][0]
span = (t1 - t0) / SEGMENTS if SEGMENTS > 0 else 0
if span > 0:
    for s in range(SEGMENTS):
        lo = t0 + s * span
        hi = lo + span
        sub = [r for r in rows if lo <= r[0] < hi]
        summarize(sub, f"seg{s+1} ({int((hi-t0)/1000)}s)")

print("\nGuia: %cpu alto + swap~0 => CPU-bound (lo esperado: GL legacy, skinning CPU).")
print("Si al achicar la ventana sube el FPS mucho => fill-rate/GPU-bound en esa parte.")
