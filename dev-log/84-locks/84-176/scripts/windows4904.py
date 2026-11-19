#!/usr/bin/env python3
"""
Read sample4904.py's output over each loop's window: the median and range of
the actual and requested frequency, RC6's residency in the window, and the
change of RPS's work, parks and wait boosts between the last boost block
before the window and the first after it, which Linux's file does not give.
A window is the pair of `time` stamps that open and close trip.py's
LOOP.loop.txt.
Usage: windows4904.py SAMPLE LOOP.loop.txt...
"""
import re
import statistics
import sys


def parse(path):
    """The samples, (t, act, cur, rc6), and the boost blocks of `path`."""
    samples, blocks, block = [], [], None
    for line in open(path):
        m = re.match(r"^([0-9.]+) (\S+) (\S+) (\S+)$", line)
        if m:
            samples.append((float(m[1]), m[2], m[3], m[4]))
            continue
        m = re.match(r"^== ([0-9.]+)", line)
        if m:
            block = {"t": float(m[1])}
            blocks.append(block)
            continue
        if block is None:
            continue
        m = re.match(r"Work runs: (\d+), up (\d+), down (\d+), timeout (\d+)", line)
        if m:
            block["work"] = tuple(int(x) for x in m.groups())
        m = re.match(r"Parks: (\d+), unparks (\d+)", line)
        if m:
            block["parks"] = tuple(int(x) for x in m.groups())
        m = re.match(r"Wait boosts: (\d+)", line)
        if m:
            block["boosts"] = int(m[1])
    return samples, [b for b in blocks if "parks" in b and "work" in b]


def window(path):
    """The first and last `time` stamps of a loop file."""
    stamps = [float(l.split()[1]) for l in open(path) if l.startswith("time ")]
    return stamps[0], stamps[-1]


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: windows4904.py SAMPLE LOOP.loop.txt...")
    samples, blocks = parse(sys.argv[1])
    for loop in sys.argv[2:]:
        a, b = window(loop)
        s = [x for x in samples if a <= x[0] <= b]
        act = [int(x[1]) for x in s if x[1] != "-"]
        cur = [int(x[2]) for x in s if x[2] != "-"]
        rc6 = [int(x[3]) for x in s if x[3] != "-"]
        before = [x for x in blocks if x["t"] <= a]
        after = [x for x in blocks if x["t"] >= b]
        if before and after:
            before, after = before[-1], after[0]
            work = tuple(y - x for x, y in zip(before["work"], after["work"]))
            counters = "parks %d, unparks %d, work %d (up %d, down %d, timeout %d), boosts %d" % (
                after["parks"][0] - before["parks"][0], after["parks"][1] - before["parks"][1],
                work[0], work[1], work[2], work[3], after["boosts"] - before["boosts"])
        else:
            counters = "no RPS counters"
        print("%s: %.1f ms, %d samples, act median %s (%d to %d), cur median %s (%d to %d), "
              "rc6 %d ms, %s" % (loop, (b - a) * 1000, len(s), statistics.median(act), min(act),
                                 max(act), statistics.median(cur), min(cur), max(cur),
                                 rc6[-1] - rc6[0] if rc6 else 0, counters))
        print("  act:", " ".join(x[1] for x in s))


if __name__ == "__main__":
    main()
