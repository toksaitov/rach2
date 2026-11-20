# compare.py DIR...: for each play that play.sh left in every DIR, the
# dropped and total frames, the system and user ticks, the big lock's wait
# in milliseconds by /proc/stat's bkl_wait_us, the holds that bkl/calls
# counted, and its six entries that waited longest, in milliseconds.
import json
import os
import sys


def stat(path):
    r = {}
    for line in open(path):
        a = line.split()
        if a and a[0] == 'cpu':
            r['user'] = int(a[1])
            r['sys'] = int(a[3])
        elif a and a[0] == 'bkl_wait_us':
            r['wait'] = int(a[1])
    return r


def calls(path):
    rows = {}
    for line in list(open(path))[1:]:
        a = line.split()
        if len(a) >= 7:
            rows[a[0] + ' ' + a[1]] = (int(a[4]), int(a[5]), int(a[6]))
    return rows


def main(dirs):
    names = sorted(n[:-5] for n in os.listdir(dirs[0]) if n.endswith('.json'))
    for n in names:
        print(n)
        for d in dirs:
            p = os.path.join(d, n)
            a, b = stat(p + '.stat0'), stat(p + '.stat1')
            r = json.load(open(p + '.json'))
            c = calls(p + '.calls')
            top = sorted(c.items(), key=lambda x: -x[1][2])[:6]
            print('  %-24s %3d/%4d sys %5d user %5d wait %5d ms holds %8d | %s' % (
                d, r['dropped'], r['total'], b['sys'] - a['sys'], b['user'] - a['user'],
                (b['wait'] - a['wait']) // 1000, sum(v[0] for v in c.values()),
                ', '.join('%s %d' % (k, v[2] // 1000) for k, v in top)))


main(sys.argv[1:])
