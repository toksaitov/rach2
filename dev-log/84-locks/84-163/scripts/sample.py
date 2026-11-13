#!/usr/bin/env python3
"""
Sample the GPU's actual frequency and the interrupt counts every 20 ms, with
the monotonic clock that trip.py stamps a loop's window with, and, with
--pin MASK, bind every thread of each tripprobe process to the processors of
MASK as the thread appears. The sampler binds itself to processor 0.
Runs as root until STOP exists. Usage: sample.py OUT STOP [--pin MASK].
OUT receives "t MHZ" lines, "== t" blocks of /proc/interrupts and the
"pin t TID NAME" lines.
"""
import glob
import os
import sys
import time


def pins(mask):
    """The processor numbers of the bit mask MASK."""
    return {i for i in range(64) if mask >> i & 1}


def trip_pids():
    """The processes whose name has trip64."""
    found = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid) as f:
                if "trip64" in f.read():
                    found.append(pid)
        except OSError:
            pass
    return found


def main():
    if len(sys.argv) not in (3, 5) or (len(sys.argv) == 5 and sys.argv[3] != "--pin"):
        raise SystemExit("usage: sample.py OUT STOP [--pin MASK]")
    out, stop = sys.argv[1], sys.argv[2]
    cpus = pins(int(sys.argv[4], 0)) if len(sys.argv) == 5 else None
    os.sched_setaffinity(0, {0})
    freq = sorted(glob.glob("/sys/class/drm/card*/gt_act_freq_mhz"))
    done, pids, scan = set(), [], 0.0
    with open(out, "w") as log:
        log.write("freq %s\n" % (freq[0] if freq else "none"))
        while not os.path.exists(stop):
            now = time.monotonic()
            if freq:
                try:
                    with open(freq[0]) as f:
                        log.write("%.6f %s\n" % (now, f.read().strip()))
                except OSError:
                    pass
            with open("/proc/interrupts") as f:
                log.write("== %.6f\n%s" % (now, f.read()))
            if cpus is not None:
                if now - scan >= 0.1:
                    pids, scan = trip_pids(), now
                for pid in pids:
                    try:
                        tids = os.listdir("/proc/%s/task" % pid)
                    except OSError:
                        continue
                    for tid in tids:
                        if tid in done:
                            continue
                        try:
                            os.sched_setaffinity(int(tid), cpus)
                            with open("/proc/%s/task/%s/comm" % (pid, tid)) as f:
                                name = f.read().strip()
                            done.add(tid)
                            log.write("pin %.6f %s %s %s\n" % (time.monotonic(), tid, name,
                                                               sorted(os.sched_getaffinity(int(tid)))))
                        except OSError:
                            pass
            time.sleep(0.02)
    return 0


if __name__ == "__main__":
    sys.exit(main())
