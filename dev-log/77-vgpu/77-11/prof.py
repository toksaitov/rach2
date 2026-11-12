# Rank the samples of debugfs's bkl/profile by entry, innermost function and
# caller chain.  Usage: prof.py NM PROFILE [N]
import bisect, collections, sys
syms = []
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
def fn(a):
    a = int(a, 16)
    if a == 0:
        return '-'
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else hex(a)
N = int(sys.argv[3]) if len(sys.argv) > 3 else 30
entry = collections.Counter(); inner = collections.Counter()
anyf = collections.Counter(); chain = collections.Counter(); tot = 0
for l in open(sys.argv[2]):
    p = l.split()
    if not p or p[0] in ('samples', 'lost'):
        print(l.strip()); continue
    n = int(p[-1]); pcs = p[-5:-1]; e = ' '.join(p[:-5])
    fs = [fn(x) for x in pcs]
    tot += n; entry[e] += n; inner[fs[0]] += n
    chain[e + ': ' + ' < '.join(fs)] += n
    for f in set(fs):
        anyf[f] += n
print('total', tot)
for title, c in (('entries', entry), ('innermost', inner), ('anywhere', anyf), ('chains', chain)):
    print('--', title)
    for k, n in c.most_common(N):
        print('%6d %5.1f%% %s' % (n, 100.0 * n / tot, k))
