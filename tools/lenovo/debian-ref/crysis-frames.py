# crysis-frames.py RUNDIR: per-loop frame times of a Crysis benchmark run from
# Gallium's HUD dump (hud/frametime_(ms)) and the counts in frames.txt.
#
# Each loop is cut as crysis.py's loop_windows cuts Rach2's
# (tests/system/games/crysis.py). frames.txt's load line, `NOW load COUNT
# UNSEEN`, gives the dump's count read after Game.log's "loading time" line
# was first seen, at uptime NOW, and the uptime of the last look that found
# none, so GAP = NOW - UNSEEN. The load's last frame L is the one frame of
# LOAD_FRAME ms or more among the frames that end less than GAP before frame
# COUNT - 1 does, the one before them and frame COUNT. It is printed apart;
# loop k is the LOOP_FRAMES frames from L + k * LOOP_FRAMES, the game's own
# count, less L in loop 0. A run whose mark came more than LOAD_LAG after
# that look, or with no such frame or more than one, is printed as uncut and
# one whose dump does not hold a loop whole fails, each with status 1. 80-2's
# cut, the LOOP_FRAMES frames before the count read as loop 0 ended, is
# printed after, as `end-cut`, since a poll's delay moves it.
import sys, os

LOOP_FRAMES = 2000
LOAD_LAG = 2.0
LOAD_FRAME = 1000


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
        gap = float(f[0]) - float(f[3]) if len(f) > 3 and f[3] != "-" else None
        mark = (int(f[2]), gap)
    else:
        ends.append((f[1], int(f[2])))
print("dump frames:", len(vals))
status = 0
if mark is None:
    print("load: uncut: no load mark in frames.txt")
    status = 1
elif mark[1] is None or mark[1] > LOAD_LAG:
    print("load: uncut: the load mark came %s after the last look that found no load, past %.1f s"
          % ("an unknown time" if mark[1] is None else "%.2f s" % mark[1], LOAD_LAG))
    status = 1
elif mark[0] >= len(vals):
    print("load: uncut: the dump's %d frames end at the load mark, frame %d" % (len(vals), mark[0]))
    status = 1
else:
    count, gap = mark
    first, ago = count - 1, 0.0
    while first > 0 and ago < gap * 1000:
        ago += vals[first]
        first -= 1
    first = max(first, 0)
    long = [i for i in range(first, count + 1) if vals[i] >= LOAD_FRAME]
    if len(long) != 1:
        print("load: uncut: %s of %d ms or more among frames %d to %d"
              % ("frames " + ", ".join("%d" % i for i in long) if long else "no frame",
                 LOAD_FRAME, first, count))
        status = 1
    else:
        last = long[0]
        print("load: last frame %d of %.1f ms, counted from 0, the one of %d ms or more in frames "
              "%d to %d, load mark at frame %d %.2f s after the last look that found no load; "
              "loop k from frame %d + %d k, frame %d left out of loop 0"
              % (last, vals[last], LOAD_FRAME, first, count, count, gap, last, LOOP_FRAMES, last))
    for k, (name, end) in enumerate(ends if len(long) == 1 else []):
        a, b = last + (1 if k == 0 else k * LOOP_FRAMES), last + (k + 1) * LOOP_FRAMES
        if b > len(vals):
            print("%s: FAILED: frames %d to %d, past the dump's %d" % (name, a, b - 1, len(vals)))
            status = 1
        else:
            stats(name, vals[a:b])
prev = None
for name, end in ends:
    start = end - LOOP_FRAMES if prev is None else prev
    seg = vals[max(start, 0):end]
    prev = end
    if seg:
        stats("end-cut " + name, seg)
sys.exit(status)
