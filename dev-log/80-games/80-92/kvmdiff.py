# KVM's counters over the load, from kvm-a.txt and kvm-b.txt, for each run
# named: those that moved, a row each.  Usage: kvmdiff.py DIR...
import sys


def rd(p):
    d = {}
    for l in open(p):
        f = l.split()
        if len(f) == 2 and f[1].lstrip('-').isdigit():
            d[f[0]] = int(f[1])
    return d


runs = [(x.split('out-', 1)[1], rd(x + '/kvm-b.txt'), rd(x + '/kvm-a.txt')) for x in sys.argv[1:]]
keys = sorted({k for _, b, a in runs for k in b if b[k] != a.get(k, 0)})
print('%-26s' % '' + ''.join('%12s' % t for t, _, _ in runs))
for k in keys:
    print('%-26s' % k + ''.join('%12d' % (b.get(k, 0) - a.get(k, 0)) for _, b, a in runs))
