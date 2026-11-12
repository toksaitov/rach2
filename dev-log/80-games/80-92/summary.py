# One line for each run: the load's time, the main thread's and llvmpipe's
# user and system ticks, every thread's, the steal ticks /proc/stat gives
# (time the host ran something else on a ready processor, which voids a
# run), and every QEMU thread's host ticks, almost all of them its vCPUs'.
# A figure whose helper failed or gave no total prints as "incomplete", with
# the helper's error on stderr, and the command exits 1.
# Usage: summary.py DIR...
import re, subprocess, sys

failed = False


def helper(name, d):
    """Run the helper NAME on D and give its lines, or None, after saying
    why on stderr, when it fails or prints nothing."""
    global failed
    r = subprocess.run([sys.executable, __file__.replace('summary.py', name), d], capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        sys.stderr.write('summary: %s: %s failed with status %d: %s\n' % (d, name, r.returncode, r.stderr.strip() or 'no output'))
        failed = True
        return None
    return r.stdout.splitlines()


def steal(p):
    """Give the steal ticks of the stat snapshot P, or None when it has none."""
    try:
        for l in open(p):
            if l.startswith('cpu '):
                return int(l.split()[8])
    except (OSError, IndexError, ValueError) as e:
        sys.stderr.write('summary: %s: %s\n' % (p, e))
    return None


for d in sys.argv[1:]:
    run = open(d.replace('out-', 'run-', 1) + '.out', errors='replace').read()
    m = re.search(r'loading time: ([0-9.]+)', run)
    main = None
    tot = lp = ('incomplete', '')
    rows = helper('tasks.py', d)
    t = re.match(r'total user (\d+) sys (\d+)', rows[0]) if rows else None
    if rows and not t:
        sys.stderr.write('summary: %s: tasks.py gave no total\n' % d)
        failed = True
    if t:
        tot = t.groups()
        lpu = lps = 0
        for r in rows[1:]:
            f = r.split()
            if f[3] == 'Main' and main is None:
                main = (f[1], f[2])
            if f[3].startswith('llvmpipe'):
                lpu += int(f[1])
                lps += int(f[2])
        lp = (str(lpu), str(lps))
    vh = ('incomplete', '')
    h = helper('hosttasks.py', d)
    for r in h or ():
        f = r.split(None, 2)
        if len(f) == 3 and f[2] == 'total':
            vh = (f[0], f[1])
    if h and vh[0] == 'incomplete':
        sys.stderr.write('summary: %s: hosttasks.py gave no total\n' % d)
        failed = True
    sa, sb = steal(d + '/stat-a.txt'), steal(d + '/stat-b.txt')
    if sa is None or sb is None:
        st = 'incomplete'
        failed = True
    else:
        st = str(sb - sa)

    def pair(p):
        return '%s+%s' % p if p[1] != '' else p[0]
    print('%-14s load %6s  main %9s llvmpipe %8s all %9s steal %4s qemu-host %9s' % (
        d.split('out-', 1)[1], m.group(1) if m else '-', pair(main or ('-', '-')), pair(lp), pair(tot), st, pair(vh)))
sys.exit(1 if failed else 0)
