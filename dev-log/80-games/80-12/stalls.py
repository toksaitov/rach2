#!/usr/bin/env python3
"""stalls.py: set the frame stalls of a Crysis run beside what sample.sh read.

Usage: stalls.py SAMPLE FRAMETIMES [SKIP]

SAMPLE is sample.sh's sample.txt and FRAMETIMES Gallium's HUD dump of frame
times in milliseconds, one a frame. Only the readings past the first SKIP
frames count, by default all but the last 6,000, the flyby's second to
fourth loops, which follow the load and the first loop's reads from the
disk. A reading holds the frames the HUD counted since the reading before
it; it is smooth when its longest frame is under 60 ms and a stall when it
is over 150 ms, or when no frame came for that long. For each group it
prints the frame rate, the big lock's wait per second of time, the
processors' split, the entries whose holds grew most, the growth of Mesa's
shader cache, the reads from the disk and HDA's underruns, so that the
cause shows in which of them moves with the stalls.
"""
import sys


def blocks(path):
    """Yield each reading of sample.txt as a dict."""
    cur = None
    for line in open(path):
        w = line.split()
        if not w:
            continue
        if w[0] == 't':
            if cur:
                yield cur
            cur = {'t': int(w[1]) / 1000.0, 'hold': {}}
        elif cur is None:
            continue
        elif w[0] == 'cpu':
            cur['user'], cur['sys'], cur['idle'] = int(w[1]), int(w[3]), int(w[4])
        elif w[0] in ('bkl_wait_us', 'bkl_wait_max_us', 'bkl_hold_max_us'):
            cur[w[0]] = int(w[1])
        elif w[0] == 'bkl_hold_entry':
            cur['hold'][w[1] + ' ' + w[2]] = int(w[4])
        elif w[0] == 'queue':
            d = dict(zip(w[1::2], w[2::2]))
            cur['underruns'] = int(d.get('underruns', 0))
            cur['missed'] = int(d.get('missed', 0))
        elif w[0] == 'frames':
            cur['frames'] = int(w[1])
        elif w[0] == 'cache':
            cur['cache'] = int(w[1]) if len(w) > 1 else 0
        elif w[0] == 'read':
            cur['read'] = int(w[1])
    if cur:
        yield cur


def delta(a, b):
    """
    What changed from reading a to reading b. /proc/stat gives only the
    sixteen entries with the largest hold totals, each a total since boot, so
    an entry's growth is taken only when both readings have it: one that has
    just entered the sixteen would otherwise charge its whole total to this
    interval. The processors' split is kept in ticks, so that a group's split
    weighs each reading by its length.
    """
    dt = b['t'] - a['t']
    holds = {}
    for k, v in b['hold'].items():
        if k in a['hold']:
            holds[k] = v - a['hold'][k]
    return {
        'dt': dt,
        'wait': (b.get('bkl_wait_us', 0) - a.get('bkl_wait_us', 0)) / 1e6,
        'user': b['user'] - a['user'],
        'sys': b['sys'] - a['sys'],
        'idle': b['idle'] - a['idle'],
        'holds': holds,
        'cache': b.get('cache', 0) - a.get('cache', 0),
        'read': (b.get('read', 0) - a.get('read', 0)) / 2048.0,
        'underruns': b.get('underruns', 0) - a.get('underruns', 0),
        'missed': b.get('missed', 0) - a.get('missed', 0),
    }


def summary(name, ds):
    """Print the sums of a group of deltas."""
    if not ds:
        print(f'{name}: none')
        return
    dt = sum(d['dt'] for d in ds)
    n = len(ds)
    holds = {}
    for d in ds:
        for k, v in d['holds'].items():
            holds[k] = holds.get(k, 0) + v
    top = sorted(holds.items(), key=lambda kv: -kv[1])[:6]
    print(f'{name}: {n} readings over {dt:.1f} s, '
          f'{sum(d["frames"] for d in ds) / dt:.1f} frames a second')
    print(f'  big-lock wait: {sum(d["wait"] for d in ds) / dt:.2f} s a second')
    ticks = sum(d['user'] + d['sys'] + d['idle'] for d in ds) or 1
    print('  processors: user {:.0%} system {:.0%} idle {:.0%}'.format(
        sum(d['user'] for d in ds) / ticks, sum(d['sys'] for d in ds) / ticks,
        sum(d['idle'] for d in ds) / ticks))
    print('  holds: ' + ', '.join(f'{k} {v / 1e6 / dt:.3f} s/s' for k, v in top))
    print(f'  shader cache: +{sum(d["cache"] for d in ds)} KiB')
    print(f'  disk: {sum(d["read"] for d in ds):.1f} MiB read')
    print(f'  HDA underruns +{sum(d["underruns"] for d in ds)}, '
          f'missed +{sum(d["missed"] for d in ds)}')


def main():
    sample, frames = sys.argv[1], sys.argv[2]
    rs = [r for r in blocks(sample) if 'user' in r and 'frames' in r]
    ft = [float(x) for x in open(frames) if x.strip()]
    skip = int(sys.argv[3]) if len(sys.argv) > 3 else len(ft) - 6000
    print(f'frames: {len(ft)}, counted past {skip}: '
          f'{sum(1 for v in ft[skip:] if v > 100)} over 100 ms, '
          f'longest {max(ft[skip:]):.0f} ms')
    smooth, stall = [], []
    for a, b in zip(rs, rs[1:]):
        if a['frames'] <= skip or b['frames'] >= len(ft):
            continue
        d = delta(a, b)
        got = ft[a['frames']:b['frames']]
        d['frames'] = len(got)
        longest = max(got) if got else d['dt'] * 1000
        if longest < 60:
            smooth.append(d)
        elif longest > 150:
            stall.append(d)
    summary('smooth, every frame under 60 ms', smooth)
    summary('stalled, a frame over 150 ms', stall)


main()
