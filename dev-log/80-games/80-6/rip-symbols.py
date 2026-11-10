# Resolve the sampler's kernel RIPs to functions.  Usage: sym.py NM-FILE < samples
import bisect, collections, sys
syms = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
out = collections.Counter()
for line in sys.stdin:
    n, rip = line.split()
    if rip == 'user':
        out['user'] += int(n)
        continue
    i = bisect.bisect_right(addrs, int(rip, 16)) - 1
    out[syms[i][1] if i >= 0 else rip] += int(n)
total = sum(out.values())
for name, n in out.most_common(40):
    print('%6d %5.1f%% %s' % (n, 100.0 * n / total, name))
