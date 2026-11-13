#!/usr/bin/env python3
"""
Split the round trips of Linux's traced loop into parts, from tracefs's text
(lin4889.sh trace) and the loop's window (trip.py's trip-gl-0.loop.txt, whose
"time" lines are the same monotonic clock). Linux's i915 signals its fences
without the dma_fence_signaled event, so a trip is taken from wined3d_cs's
side: each sleep of wined3d_cs whose wake (sched_waking) comes from an i915
interrupt, that is on the interrupt's processor within 1 ms of an
irq_handler_entry of i915, is split into the GPU's work and the interrupt's
delivery (wined3d_cs's last i915_request_add before the sleep to the
interrupt's entry), the interrupt's work up to the wake (entry to
sched_waking) and the wake's latency (sched_waking to the switch-in, which
sched_wakeup divides into the wake's own work and the ready wait).
Usage: split.py TRACE LOOP [COMM]
"""
import bisect
import re
import sys

LINE = re.compile(r"^\s*(.+?)-(\d+)\s+\[(\d+)\]\s+(\S+)\s+([0-9.]+):\s+(\w+):\s*(.*)$")


def window(path):
    """The loop's start and end on the monotonic clock, in seconds."""
    times = [float(l.split()[1]) for l in open(path) if l.startswith("time ")]
    return times[0], times[-1]


def stats(name, values):
    """One line of microseconds: count, mean, median, p90, sum."""
    if not values:
        return "%s: none" % name
    v = sorted(values)
    return "%s: %d, mean %.1f us, median %.1f, p90 %.1f, max %.1f, sum %.1f ms" % (
        name, len(v), sum(v) / len(v), v[len(v) // 2], v[int(len(v) * 0.9)], v[-1], sum(v) / 1000)


def main():
    trace, loop = sys.argv[1], sys.argv[2]
    who = sys.argv[3] if len(sys.argv) > 3 else "wined3d_cs"
    t0, t1 = window(loop)
    adds, irqs, wakings, wakeups, ins, outs = [], [], [], [], [], []
    cs = None
    for line in open(trace, errors="replace"):
        m = LINE.match(line)
        if not m:
            continue
        t = float(m.group(5))
        if t < t0 - 0.01 or t > t1:
            continue
        comm, pid, cpu, ev, rest = m.group(1).strip(), int(m.group(2)), int(m.group(3)), m.group(6), m.group(7)
        us = t * 1e6
        f = dict(re.findall(r"(\w+)=([^ ,]+)", rest))
        if ev == "i915_request_add" and comm == who:
            cs = pid
            adds.append(us)
        elif ev == "irq_handler_entry" and f.get("name") == "i915":
            irqs.append((us, cpu))
        elif ev == "sched_waking" and f.get("comm") == who:
            wakings.append((us, cpu, comm))
        elif ev == "sched_wakeup" and f.get("comm") == who:
            wakeups.append((us, int(f["target_cpu"])))
        elif ev == "sched_switch":
            if f.get("next_comm") == who:
                ins.append((us, cpu))
            if f.get("prev_comm") == who:
                outs.append((us, f.get("prev_state"), cpu))
    print("window %.1f ms, %s %s: %d request adds, %d i915 interrupts, %d sleeps, %d wakes" % (
        (t1 - t0) * 1000, who, cs, len([a for a in adds if a >= t0 * 1e6]),
        len([i for i in irqs if i[0] >= t0 * 1e6]),
        len([o for o in outs if o[1] in ("S", "D") and o[0] >= t0 * 1e6]), len(wakings)))
    by = {}
    for _, c in irqs:
        by[c] = by.get(c, 0) + 1
    print("i915 interrupts by processor: %s" % sorted(by.items()))
    irq_t = [t for t, _ in irqs]
    add_t = adds
    in_t = [t for t, _ in ins]
    up_t = [t for t, _ in wakeups]
    out_t = [t for t, _, _ in outs]
    gpu, isr, wk, ready, slept, other, same = [], [], [], [], [], [], 0
    first = t0 * 1e6
    for tw, wcpu, wcomm in wakings:
        if tw < first:
            continue
        o = bisect.bisect_right(out_t, tw) - 1
        if o < 0 or outs[o][1] not in ("S", "D"):
            continue
        n = bisect.bisect_left(in_t, tw)
        if n >= len(in_t):
            continue
        tin = in_t[n]
        i = bisect.bisect_right(irq_t, tw) - 1
        while i >= 0 and irqs[i][1] != wcpu and tw - irq_t[i] < 1000:
            i -= 1
        if i < 0 or irqs[i][1] != wcpu or tw - irq_t[i] >= 1000 or irq_t[i] < out_t[o] - 2000:
            other.append(tin - tw)
            continue
        a = bisect.bisect_right(add_t, out_t[o]) - 1
        if a < 0:
            continue
        u = bisect.bisect_left(up_t, tw)
        tup = up_t[u] if u < len(up_t) and up_t[u] <= tin else tin
        if ins[n][1] == wcpu:
            same += 1
        gpu.append(irq_t[i] - add_t[a])
        isr.append(tw - irq_t[i])
        wk.append(tup - tw)
        ready.append(tin - tup)
        slept.append(tin - out_t[o])
    print(stats("sleeps woken by an i915 interrupt, asleep for", slept))
    print(stats("  request add to i915 interrupt entry (GPU and delivery)", gpu))
    print(stats("  interrupt entry to sched_waking", isr))
    print(stats("  sched_waking to sched_wakeup", wk))
    print(stats("  sched_wakeup to switch-in (ready wait)", ready))
    print("  woken on the interrupt's processor and run there: %d" % same)
    print(stats("other sleeps' waking to switch-in", other))
    return 0


if __name__ == "__main__":
    sys.exit(main())
