# crysis-frames.py RUNDIR: per-loop frame times of a Crysis benchmark run from
# Gallium's HUD dump (hud/frametime_(ms)) and the counts in frames.txt.
#
# Each loop is cut as crysis.py's loop_windows cuts Rach2's
# (tests/system/games/crysis.py): the load's last frame is the longest of the
# LOAD_SPAN frames up to the load mark, the dump's count when Game.log's
# "loading time" line was first seen; it is printed apart, and loop k is the
# LOOP_FRAMES frames from the one after it plus k loops. The cut of 80-2's
# figures, the LOOP_FRAMES frames before the count read as loop 0 ended, is
# printed after, as `end-cut`, since a poll's delay moves it.
import sys, os

LOOP_FRAMES = 2000
LOAD_SPAN = 60


def stats(name, seg):
    """Print one line of SEG's figures, in ms, under NAME."""
    s = sorted(seg)
    p = lambda q: s[min(len(s) - 1, int(q * len(s)))]
    print("%s: frames %d, mean %.1f ms, median %.1f, p99 %.1f, max %.1f, >50 ms %d, >100 ms %d, >250 ms %d"
          % (name, len(seg), sum(seg) / len(seg), p(0.5), p(0.99), s[-1],
             sum(1 for v in seg if v > 50), sum(1 for v in seg if v > 100),
             sum(1 for v in seg if v > 250)))


d = sys.argv[1]
vals = []
for line in open(os.path.join(d, "hud", "frametime_(ms)")):
    f = line.split()
    if f:
        vals.append(float(f[-1]))
mark, ends = None, []
for line in open(os.path.join(d, "frames.txt")):
    f = line.split()
    if f[1] == "load":
        mark = int(f[2])
    else:
        ends.append((f[1], int(f[2])))
print("dump frames:", len(vals))
if mark is None or mark > len(vals) or mark == 0:
    print("load: no load mark in frames.txt")
else:
    last = max(range(max(mark - LOAD_SPAN, 0), mark), key=lambda i: vals[i])
    print("load: last frame %d of %.1f ms, load mark at frame %d; loop 0 from frame %d, %d frames a loop"
          % (last, vals[last], mark, last + 1, LOOP_FRAMES))
    for k, (name, end) in enumerate(ends):
        seg = vals[last + 1 + k * LOOP_FRAMES:last + 1 + (k + 1) * LOOP_FRAMES]
        if seg:
            stats(name, seg)
prev = None
for name, end in ends:
    start = end - LOOP_FRAMES if prev is None else prev
    seg = vals[max(start, 0):end]
    prev = end
    if seg:
        stats("end-cut " + name, seg)
