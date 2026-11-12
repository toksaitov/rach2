# One line for each run: the load's time, the main thread's and llvmpipe's
# user and system ticks, every thread's, the steal ticks /proc/stat gives
# (time the host ran something else on a ready processor, which voids a
# run), and every QEMU thread's host ticks, almost all of them its vCPUs'.
# Usage: summary.py DIR...
import re, subprocess, sys


def steal(p):
    for l in open(p):
        if l.startswith('cpu '):
            return int(l.split()[8])
    return 0


for d in sys.argv[1:]:
    run = open(d.replace('out-', 'run-', 1) + '.out', errors='replace').read()
    m = re.search(r'loading time: ([0-9.]+)', run)
    rows = subprocess.run([sys.executable, __file__.replace('summary.py', 'tasks.py'), d], capture_output=True, text=True).stdout.splitlines()
    tot = re.match(r'total user (\d+) sys (\d+)', rows[0]).groups() if rows else ('-', '-')
    main = lp = None
    lpu = lps = 0
    for r in rows[1:]:
        f = r.split()
        if f[3] == 'Main' and main is None:
            main = (f[1], f[2])
        if f[3].startswith('llvmpipe'):
            lpu += int(f[1])
            lps += int(f[2])
    h = subprocess.run([sys.executable, __file__.replace('summary.py', 'hosttasks.py'), d], capture_output=True, text=True).stdout.splitlines()
    vu = vs = 0
    for r in h:
        f = r.split(None, 2)
        if len(f) == 3 and f[2] == 'total':
            vu, vs = int(f[0]), int(f[1])
    st = steal(d + '/stat-b.txt') - steal(d + '/stat-a.txt')
    print('%-14s load %6s  main %4s+%-4s llvmpipe %4d+%-3d all %4s+%-4s steal %4d qemu-host %4d+%-4d' % (
        d.split('out-', 1)[1], m.group(1) if m else '-', *(main or ('-', '-')), lpu, lps, *tot, st, vu, vs))
