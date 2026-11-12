# A vkcube round's processor time from the guest's /proc/stat before and
# after it. Usage: stat.py PATH [FRAMES]: PATH is a profile.sh result
# directory, whose measured/stat-before and stat-after bound its long round,
# or a runner's cube-N/record, whose kernel_before and kernel_after lines
# bound that round; FRAMES is the round's presents (3346 for a directory,
# 346 for a record).
import os
import sys


def cpu(lines):
    """The cpu line's counts and the number of processors in /proc/stat."""
    total, n = None, 0
    for l in lines:
        if l.startswith('cpu '):
            total = [int(x) for x in l.split()[1:]]
        elif l.startswith('cpu'):
            n += 1
    return total, n


path = sys.argv[1]
if os.path.isdir(path):
    a, n = cpu(open(path + '/measured/stat-before'))
    b, _ = cpu(open(path + '/measured/stat-after'))
    frames = 3346
else:
    lines = open(path).read().splitlines()
    a, n = cpu(l.split(' ', 1)[1] for l in lines if l.startswith('kernel_before '))
    b, _ = cpu(l.split(' ', 1)[1] for l in lines if l.startswith('kernel_after '))
    frames = 346
if len(sys.argv) > 2:
    frames = int(sys.argv[2])
v = [y - x for x, y in zip(a, b)]
user, system, idle = v[0] + v[1], v[2] + v[5] + v[6], v[3] + v[4]
total = user + system + idle + v[7]
wall = total / n / 100.0
print('%s: round %.1f s, user %.2f s, system %.2f s (%.2f ms a frame), busy %.1f%% of %d, '
      '%.1f frames a second' % (path, wall, user / 100.0, system / 100.0, system * 10.0 / frames,
                                100.0 * (user + system) / total, n, frames / wall))
