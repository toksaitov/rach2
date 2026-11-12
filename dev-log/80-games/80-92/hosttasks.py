# Each QEMU thread's host CPU over the load, in ticks of the host's clock
# (100 a second), from host-a.txt and host-b.txt.  Usage: hosttasks.py DIR
import re, sys


def rd(p):
    d = {}
    for l in open(p):
        tid, rest = l.split(' ', 1)
        m = re.match(r'\d+ \((.*)\) \S (.*)', rest)
        if m:
            f = m.group(2).split()
            d[tid] = (m.group(1), int(f[10]), int(f[11]))
    return d


a = rd(sys.argv[1] + '/host-a.txt')
b = rd(sys.argv[1] + '/host-b.txt')
tu = ts = 0
for k, (c, u, s) in sorted(b.items(), key=lambda kv: kv[1][0]):
    u0, s0 = a[k][1:] if k in a else (0, 0)
    tu += u - u0
    ts += s - s0
    if u - u0 + s - s0:
        print('%6d %6d %s' % (u - u0, s - s0, c))
print('%6d %6d total' % (tu, ts))
