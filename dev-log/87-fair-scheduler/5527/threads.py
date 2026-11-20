"""threads.py BEFORE AFTER: the threads' run and run-queue wait over a play,
from two snapshots of threads.sh, summed by process and thread name, the
largest waits first. A thread that started during the play counts whole."""
import collections
import sys


def read(path):
    d = {}
    for line in open(path):
        f = line.split()
        if len(f) == 6:
            d[(f[0], f[1])] = (f[2], int(f[3]), int(f[4]), int(f[5]))
    return d


a, b = read(sys.argv[1]), read(sys.argv[2])
pname = {p: c for (p, t), (c, *_r) in b.items() if p == t}
sums = collections.defaultdict(lambda: [0, 0, 0, 0])
for k, (c, run, wait, sl) in b.items():
    r0, w0, s0 = a[k][1:] if k in a else (0, 0, 0)
    key = (pname.get(k[0], "?"), c)
    s = sums[key]
    s[0] += run - r0
    s[1] += wait - w0
    s[2] += sl - s0
    s[3] += 1
tot = [sum(v[i] for v in sums.values()) for i in range(3)]
print("%-16s %-18s %4s %9s %9s %8s" % ("process", "thread", "n", "run ms", "wait ms", "slices"))
print("%-16s %-18s %4s %9.0f %9.0f %8d" % ("all", "", "", tot[0] / 1e6, tot[1] / 1e6, tot[2]))
for (p, c), (run, wait, sl, n) in sorted(sums.items(), key=lambda x: -x[1][1])[:int(sys.argv[3]) if len(sys.argv) > 3 else 40]:
    if run or wait:
        print("%-16s %-18s %4d %9.0f %9.0f %8d" % (p[:16], c[:18], n, run / 1e6, wait / 1e6, sl))
