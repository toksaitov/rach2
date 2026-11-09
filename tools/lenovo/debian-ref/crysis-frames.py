# crysis-frames.py RUNDIR: per-loop frame times of a Crysis benchmark run from
# Gallium's HUD dump (hud/frametime_(ms)) and the loop ends in frames.txt.
import sys, os

d = sys.argv[1]
vals = []
for line in open(os.path.join(d, "hud", "frametime_(ms)")):
    f = line.split()
    if f:
        vals.append(float(f[-1]))
ends = []
for line in open(os.path.join(d, "frames.txt")):
    f = line.split()
    ends.append((f[1], int(f[2])))
print("dump frames:", len(vals))
prev = None
for name, end in ends:
    start = end - 2000 if prev is None else prev
    seg = vals[max(start, 0):end]
    prev = end
    if not seg:
        continue
    s = sorted(seg)
    p = lambda q: s[min(len(s) - 1, int(q * len(s)))]
    print("%s: frames %d, mean %.1f ms, median %.1f, p99 %.1f, max %.1f, >50 ms %d, >100 ms %d, >250 ms %d"
          % (name, len(seg), sum(seg) / len(seg), p(0.5), p(0.99), s[-1],
             sum(1 for v in seg if v > 50), sum(1 for v in seg if v > 100),
             sum(1 for v in seg if v > 250)))
