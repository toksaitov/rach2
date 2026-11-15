#!/usr/bin/env python3
"""
Sample the GPU's clock every 20 ms on the monotonic clock that trip.py
stamps a loop's window with: gt_act_freq_mhz, gt_cur_freq_mhz and RC6's
residency each time, and every third time i915_rps_boost_info, whose last
lines give RPS's work runs and events, its parks and unparks, and
GEN6_RPNSWREQ against the actual frequency. Bound to processor 0. Runs as
root until STOP exists. Usage: sample4904.py OUT STOP.
OUT receives "t ACT CUR RC6" lines and "== t" blocks of the debugfs file.
"""
import glob
import os
import sys
import time


def first(pattern):
    """The first path that `pattern` matches, or None."""
    found = sorted(glob.glob(pattern))
    return found[0] if found else None


def read(path):
    """The text of `path` stripped, or "-" when it cannot be read."""
    try:
        with open(path) as f:
            return f.read().strip()
    except (OSError, TypeError):
        return "-"


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: sample4904.py OUT STOP")
    out, stop = sys.argv[1], sys.argv[2]
    os.sched_setaffinity(0, {0})
    card = first("/sys/class/drm/card*/gt_act_freq_mhz")
    card = os.path.dirname(card) if card else None
    act = card and card + "/gt_act_freq_mhz"
    cur = card and card + "/gt_cur_freq_mhz"
    rc6 = card and card + "/power/rc6_residency_ms"
    boost = first("/sys/kernel/debug/dri/*/i915_rps_boost_info")
    n = 0
    with open(out, "w") as log:
        log.write("card %s boost %s\n" % (card or "none", boost or "none"))
        while not os.path.exists(stop):
            now = time.monotonic()
            log.write("%.6f %s %s %s\n" % (now, read(act), read(cur), read(rc6)))
            if boost and n % 3 == 0:
                log.write("== %.6f\n%s\n" % (now, read(boost)))
            n += 1
            time.sleep(0.02)
    return 0


if __name__ == "__main__":
    sys.exit(main())
