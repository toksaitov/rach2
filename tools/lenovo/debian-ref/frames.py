"""frames.py: frame-time percentiles from Gallium's HUD dump.

Usage: python3 -I frames.py DUMP [SKIP_S [SPAN_S]]

DUMP is the file GALLIUM_HUD_DUMP_DIR receives for `frametime`, one value in
milliseconds per frame. The measured window starts SKIP_S seconds (default
5) after the last frame of 1 s or more in the dump's first two minutes, the
end of loading, and lasts SPAN_S seconds (default: to the end of the dump).
Prints the window and its frame count, mean frame rate, median, 90th, 99th
and 99.9th percentiles, the worst frame, and the frames over 33.3, 50 and
100 ms ("drops" at the panel's 60 Hz are frames over 16.7 ms, counted too).
"""
import sys


def main():
    vals = []
    for line in open(sys.argv[1]):
        f = line.split()
        if f:
            vals.append(float(f[-1]))
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    span = float(sys.argv[3]) if len(sys.argv) > 3 else None
    t, last_long = 0.0, -1
    for i, v in enumerate(vals):
        if t > 120000:
            break
        if v >= 1000:
            last_long = i
        t += v
    i, t = last_long + 1, 0.0
    while i < len(vals) and t < skip * 1000:
        t += vals[i]
        i += 1
    start, t = i, 0.0
    while i < len(vals) and (span is None or t < span * 1000):
        t += vals[i]
        i += 1
    seg = vals[start:i]
    if not seg:
        print("frames: no frames in the window (dump %d frames)" % len(vals))
        return
    s = sorted(seg)

    def p(q):
        return s[min(len(s) - 1, int(q * len(s)))]
    total = sum(seg)
    print("dump %d frames; window frames %d..%d, %.1f s" % (len(vals), start, i, total / 1000))
    print("mean %.2f FPS, median %.2f ms, p90 %.2f, p99 %.2f, p99.9 %.2f, max %.1f"
          % (len(seg) * 1000 / total, p(0.5), p(0.9), p(0.99), p(0.999), s[-1]))
    print("frames over 16.7 ms %d, over 33.3 ms %d, over 50 ms %d, over 100 ms %d"
          % tuple(sum(1 for v in seg if v > x) for x in (16.7, 33.3, 50, 100)))


main()
