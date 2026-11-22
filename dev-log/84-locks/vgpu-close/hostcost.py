#!/usr/bin/env python3
"""hostcost.py: how long QEMU spends on each CTX_DETACH_RESOURCE and
RESOURCE_UNREF in a trace of task 5676's measurement, and how many of a
burst's notifies reach QEMU.

    python3 hostcost.py TRACE

QEMU's virtio-gpu traces each command as it begins to run it, and runs the
commands it took from the queue one after the other on its main thread, so
the time from one command's event to the next one's is the first command's
cost when the two lie less than 1 ms apart. Prints, for DETACH and UNREF,
how many such pairs there are and the median and 90th percentile of their
gaps in microseconds, taken over the whole run. Then, for the second,
third and fourth submissions, the burst of closes after each: from its
second DETACH, after the first close's wait for the submission, to its last
UNREF, its length, its DETACHes and the notifies QEMU took in it
(virtio_pci_notify_write, the writes it handles itself).
"""
import datetime
import sys

PAIR = 0.001


def stamp(word):
    return datetime.datetime.strptime(word, "%Y-%m-%dT%H:%M:%S.%fZ").timestamp()


def main():
    cmds, subs, notifies = [], [], []
    for line in open(sys.argv[1], errors="replace"):
        w = line.split()
        if len(w) < 2:
            continue
        if w[1].startswith("virtio_gpu_cmd_"):
            cmds.append((stamp(w[0]), w[1][len("virtio_gpu_cmd_"):]))
            if w[1] == "virtio_gpu_cmd_ctx_submit":
                subs.append(stamp(w[0]))
        elif w[1] == "virtio_pci_notify_write":
            notifies.append(stamp(w[0]))
    print("# command pairs median_us p90_us")
    for kind in ("ctx_res_detach", "res_unref"):
        gaps = sorted((b[0] - a[0]) * 1e6 for a, b in zip(cmds, cmds[1:])
                      if a[1] == kind and b[0] - a[0] < PAIR)
        if gaps:
            print("%s %d %.1f %.1f" % (kind, len(gaps), gaps[len(gaps) // 2],
                                       gaps[min(len(gaps) - 1, len(gaps) * 9 // 10)]))
    print("# submission burst_ms detaches notifies notifies_a_detach")
    for i in range(1, min(4, len(subs) - 1)):
        lo, hi = subs[i], subs[i + 1]
        d = [t for t, k in cmds if k == "ctx_res_detach" and lo <= t < hi]
        u = [t for t, k in cmds if k == "res_unref" and lo <= t < hi]
        if len(d) < 2 or not u:
            continue
        a, b = d[1], max(u)
        n = sum(1 for t in notifies if a <= t <= b)
        nd = sum(1 for t in d if a <= t <= b)
        print("%d %.1f %d %d %.2f" % (i + 1, (b - a) * 1e3, nd, n, float(n) / nd))


if __name__ == "__main__":
    main()
