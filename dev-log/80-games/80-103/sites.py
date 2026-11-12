#!/usr/bin/env python3
# sites.py OUT DEBUG: the difference of the last two irqsites dumps in OUT,
# by chain, symbolized, and the phase counters' difference.
import re, subprocess, sys, collections
txt = open(sys.argv[1]).read()
blocks = re.split(r'^== ', txt, flags=re.M)
dumps = [b for b in blocks if b.startswith('cat /sys/kernel/debug/irqsites')]
def parse(b):
    d = collections.Counter(); ph = collections.defaultdict(lambda: [0, 0]); cpu = 0
    for l in b.splitlines():
        f = l.split()
        if len(f) == 5 and f[0].isdigit():
            d[tuple(f[1:])] += int(f[0])
        elif f[:1] == ['lost']:
            cpu += 1
        elif f[:1] == ['phase']:
            ph[int(f[1])][0] += int(f[2], 16); ph[int(f[1])][1] += int(f[3])
    return d, ph
a, pa = parse(dumps[-2]); b, pb = parse(dumps[-1])
d = b - a
addrs = sorted({x for k in d for x in k if x != '0' * 16})
sym = {}
if addrs:
    r = subprocess.run(['addr2line', '-f', '-s', '-e', sys.argv[2]] + ['0x' + x for x in addrs], capture_output=True, text=True).stdout.splitlines()
    for i, x in enumerate(addrs):
        sym[x] = '%s:%s' % (r[2 * i], r[2 * i + 1].split(' ')[0])
def name(x):
    return sym.get(x, '-')
tot = sum(d.values())
print('total', tot)
mode = sys.argv[3] if len(sys.argv) > 3 else 'chain'
agg = collections.Counter()
for k, v in d.items():
    if mode == 'pc':
        agg[name(k[0])] += v
    elif mode == 'r1':
        agg[(name(k[0]).split(':')[0], name(k[1]))] += v
    elif mode == 'cls':
        agg[(name(k[0]).split(':')[0], name(k[1]), int(k[3], 16))] += v
    elif mode == 'r2':
        agg[(name(k[0]).split(':')[0], name(k[1]), name(k[2]))] += v
    else:
        agg[tuple(name(x) for x in k)] += v
for k, v in agg.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 40):
    print('%6d' % v, k)
hz = 1
for k in range(8):
    print('phase', k, 'cycles', pb[k][0] - pa[k][0], 'n', pb[k][1] - pa[k][1])
def hits(b):
    h = collections.Counter()
    for l in b.splitlines():
        f = l.split()
        if f[:1] == ['hit']:
            h[int(f[1])] += int(f[2])
    return h
ha = hits(dumps[-2]); hb = hits(dumps[-1])
names = ['before', 'lock', 'select', 'drop+wait', 'switch', 'tail', 'membarrier', 'after', 'unstamped']
for c, cn in enumerate(['ipi', 'timer', 'device']):
    row = [hb[c * 9 + k] - ha[c * 9 + k] for k in range(9)]
    if cn == 'ipi':
        print(cn, ' '.join('%s=%d' % (names[k], row[k]) for k in range(9)))
    else:
        print(cn, row[0])
print('device row: wakecls 1 ipi 2 timer 3 dev; 6 = kick sent before halt ended', [hb[18 + k] - ha[18 + k] for k in range(9)])
print('timer row', [hb[9 + k] - ha[9 + k] for k in range(9)])
print('kick-state row (1 halted, 2 idle awake, 3 busy; +4 many)', [hb[27 + k] - ha[27 + k] for k in range(9)])
def lats(b):
    h = collections.Counter()
    for l in b.splitlines():
        f = l.split()
        if f[:1] == ['lat']:
            h[int(f[1])] += int(f[2])
    return h
la = lats(dumps[-2]); lb = lats(dumps[-1])
print('landed-in-schedule latency us buckets <1,<2,<4..', [lb[k] - la[k] for k in range(12)])
print('all IPIs latency us buckets', [lb[12 + k] - la[12 + k] for k in range(12)])
def tk(b):
    h = collections.Counter()
    for l in b.splitlines():
        f = l.split()
        if f[:1] == ['tickat']:
            h[int(f[1])] += int(f[2])
    return h
ta = tk(dumps[-2]); tb = tk(dumps[-1])
print('tick due in phase', ' '.join('%s=%d' % (names[k], tb[k] - ta[k]) for k in range(8)))
def cl(b):
    h = collections.defaultdict(lambda: [0, 0])
    for l in b.splitlines():
        f = l.split()
        if f[:1] == ['caller']:
            h[(f[1], f[4])][0] += int(f[2], 16); h[(f[1], f[4])][1] += int(f[3])
    return h
ca = cl(dumps[-2]); cb = cl(dumps[-1])
rows = []
for k, v in cb.items():
    d0 = v[0] - ca.get(k, [0, 0])[0]; d1 = v[1] - ca.get(k, [0, 0])[1]
    ra = int(k[0], 16)
    r = subprocess.run(['addr2line', '-f', '-s', '-e', sys.argv[2], hex(ra)], capture_output=True, text=True).stdout.split()
    rows.append((d0, d1, k[1], r[0], r[1]))
for r in sorted(rows, reverse=True)[:15]:
    print('bkl wait %12d cycles %7d takes IF %s %s %s' % r)
def bb(b):
    h = collections.Counter()
    for l in b.splitlines():
        f = l.split()
        if f[:1] == ['b2b']:
            h[int(f[1])] += int(f[2])
    return h
ba = bb(dumps[-2]); bz = bb(dumps[-1])
print('back-to-back after: tick=%d ipi=%d dev=%d tick-waited=%d ipi-waited=%d dev-waited=%d entries=%d' % tuple(bz[k] - ba[k] for k in (0, 1, 2, 3, 4, 5, 7)))
