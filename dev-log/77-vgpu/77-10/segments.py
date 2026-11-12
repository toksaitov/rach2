#!/usr/bin/env python3
"""segments.py: split a wsl-cube.sh trace into vkcube's frames and time
their steps.

    python3 -I segments.py TRACE

A frame runs from one RESOURCE_FLUSH to the next, within the runs of flushes
less than 50 ms apart that hold 200 or more, leaving out each run's first
and last ten. Weston's context is the one whose first submission is 4316
bytes; vkcube's is every other. The steps, in milliseconds, means and
medians:

    A  from the flush to vkcube's first submission
    B  from vkcube's last submission to Weston's first
    C  between Weston's first submission and its last
    D  from Weston's last submission to the last fence answer before the
       SET_SCANOUT
    E  from that answer to the SET_SCANOUT
"""
import datetime
import statistics
import sys


def stamp(text):
    """Seconds of a trace line's ISO time."""
    return datetime.datetime.fromisoformat(text.replace('Z', '+00:00')).timestamp()


def main():
    events = []
    for line in open(sys.argv[1]):
        part = line.split(None, 2)
        try:
            events.append((stamp(part[0]), part[1].replace('virtio_gpu_', ''),
                           part[2].strip() if len(part) > 2 else ''))
        except (IndexError, ValueError):
            pass
    weston = None
    for e in events:
        if e[1] == 'cmd_ctx_submit' and 'size 4316' in e[2]:
            weston = e[2].split(',')[0]
            break
    flushes = [i for i, e in enumerate(events) if e[1] == 'cmd_res_flush']
    runs, run = [], [flushes[0]]
    for a, b in zip(flushes, flushes[1:]):
        if events[b][0] - events[a][0] < 0.05:
            run.append(b)
        else:
            runs.append(run)
            run = [b]
    runs.append(run)
    steps = {k: [] for k in ('total', 'A', 'B', 'C', 'D', 'E')}
    submits = []
    for run in runs:
        if len(run) < 200:
            continue
        for a, b in zip(run[10:-10], run[11:-9]):
            frame = events[a:b + 1]
            t0 = frame[0][0]
            sub = [e for e in frame if e[1] == 'cmd_ctx_submit']
            cube = [e for e in sub if not e[2].startswith(weston)]
            wes = [e for e in sub if e[2].startswith(weston)]
            scan = [e for e in frame if e[1] == 'cmd_set_scanout']
            if len(wes) < 2 or not cube or not scan:
                continue
            before = [e for e in cube if e[0] < wes[0][0]]
            if not before:
                continue
            answers = [e for e in frame if e[1] == 'fence_resp' and e[0] < scan[-1][0]]
            last = max(answers[-1][0] if answers else wes[-1][0], wes[-1][0])
            steps['total'].append(frame[-1][0] - t0)
            steps['A'].append(before[0][0] - t0)
            steps['B'].append(wes[0][0] - before[-1][0])
            steps['C'].append(wes[-1][0] - wes[0][0])
            steps['D'].append(last - wes[-1][0])
            steps['E'].append(scan[-1][0] - last)
            submits.append(len(cube))
    print('frames %d, vkcube submissions a frame %.2f' % (len(submits), statistics.mean(submits)))
    for k, v in steps.items():
        print('%-5s mean %6.3f median %6.3f' % (k, 1000 * statistics.mean(v),
                                                1000 * statistics.median(v)))


if __name__ == '__main__':
    main()
