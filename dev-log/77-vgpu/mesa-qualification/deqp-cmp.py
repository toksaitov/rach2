# deqp-cmp.py A B: compare two results.csv files of deqp-run.py by case; print
# the count of each change of status from A to B and the cases that changed.
import collections, sys
a = dict(l.strip().split(',', 1) for l in open(sys.argv[1]) if l.strip())
b = dict(l.strip().split(',', 1) for l in open(sys.argv[2]) if l.strip())
moves = collections.Counter((a.get(c, 'Missing'), b.get(c, 'Missing')) for c in sorted(set(a) | set(b)))
for (x, y), n in sorted(moves.items()):
    print('%-14s -> %-14s %6d' % (x, y, n))
for c in sorted(set(a) | set(b)):
    if a.get(c) != b.get(c):
        print('  %s: %s -> %s' % (c, a.get(c, 'Missing'), b.get(c, 'Missing')))
