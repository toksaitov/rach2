#!/usr/bin/env python3
"""
The GPU's actual frequency and the i915 interrupts over a loop's window, from
sample.py's output and trip.py's loop file, both on the monotonic clock.
Usage: window.py SAMPLES LOOP
"""
import sys

def main():
    samples, loop = sys.argv[1], sys.argv[2]
    times = [float(l.split()[1]) for l in open(loop) if l.startswith("time ")]
    t0, t1 = times[0], times[-1]
    freq, blocks, cur = [], [], None
    for line in open(samples):
        f = line.split()
        if line.startswith("== "):
            cur = [float(f[1]), {}]
            blocks.append(cur)
        elif cur is not None and "i915" in line and ":" in f[0]:
            nums = [int(x) for x in f[1:] if x.isdigit()]
            cur[1] = nums
        elif len(f) == 2 and f[0][0].isdigit() and "." in f[0]:
            t = float(f[0])
            if t0 <= t <= t1:
                freq.append(int(f[1]))
    print("window %.1f ms" % ((t1 - t0) * 1000))
    if freq:
        freq.sort()
        print("gt_act_freq_mhz: %d samples, min %d, median %d, mean %.0f, max %d" % (
            len(freq), freq[0], freq[len(freq) // 2], sum(freq) / len(freq), freq[-1]))
    inside = [b for b in blocks if t0 <= b[0] <= t1]
    before = [b for b in blocks if b[0] < t0]
    after = [b for b in blocks if b[0] > t1]
    if before and after:
        a, b = before[-1], after[0]
        d = [y - x for x, y in zip(a[1], b[1])]
        print("i915 interrupts from %.1f ms before to %.1f ms after: %s" % (
            (t0 - a[0]) * 1000, (b[0] - t1) * 1000, d))
    return 0

if __name__ == "__main__":
    sys.exit(main())
