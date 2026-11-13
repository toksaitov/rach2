# classes.py OUT...: the EOI writes of each tar in a wait-diag.patch run, by
# what waited: a pair with the tick, a kick with the disk, nothing seen.
import sys
for f in sys.argv[1:]:
    blocks = []; cur = None
    for l in open(f):
        if l.startswith('== cat'):
            cur = {}; blocks.append(cur); continue
        if l.startswith('=='):
            cur = None; continue
        if cur is None: continue
        x = l.split()
        if len(x) > 2 and x[2].isdigit():
            cur[(x[0], x[1])] = list(map(int, x[2:]))
    for i in range(1, len(blocks)):
        a, b = blocks[i - 1], blocks[i]
        d = lambda k: [p - q for p, q in zip(b.get(k, [0] * 5), a.get(k, [0] * 5))]
        c = {'tick': 0, 'kick+disk': 0, 'none': 0, 'other': 0}
        idle = 0
        for k in b:
            if k[0] != 'eoiwait': continue
            n = int(k[1], 16); v = n & 0xff; w = (n >> 8) & 0xff; ctx = (n >> 16) & 15
            m = d(k)[0]
            if ctx == 0: idle += m
            if 0x20 in (v, w): c['tick'] += m
            elif w == 0: c['none'] += m
            elif {v, w} == {0xf0, 0x31}: c['kick+disk'] += m
            else: c['other'] += m
        t = d(('wrmsr', '0x80b'))
        taken = sum(d(k)[0] for k in b if k[0] == 'eoitaken')
        print(f, 'tar', i, 'eoi', t[0], 'cpus', t[1:], 'taken', taken, c, 'idle', idle)
