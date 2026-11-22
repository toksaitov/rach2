#!/usr/bin/env python3
"""notifies.py: which commands the notifies of a burst of closes carry, in a
trace of task 5676's measurement with the events of `events`.

    python3 notifies.py TRACE

QEMU's virtio-gpu takes the commands a notify tells it of in one pass and
traces each as it begins to run it, so the commands between one notify
write (virtio_pci_notify_write) and the next are those that notify carried,
and a command traced before any notify of the burst rode on an earlier one.
For the second to fourth submissions, the burst of closes after each, from
its second DETACH to its last UNREF, as hostcost.py takes it: the burst's
DETACHes and UNREFs; its notifies; how many carried DETACHes alone, UNREFs
alone, both, or nothing; and the notifies a DETACH, the commands a notify
and the longest run of UNREFs one notify carried.
"""
import datetime
import sys


def stamp(word):
    return datetime.datetime.strptime(word, "%Y-%m-%dT%H:%M:%S.%fZ").timestamp()


def main():
    events, subs = [], []
    for line in open(sys.argv[1], errors="replace"):
        w = line.split()
        if len(w) < 2:
            continue
        if w[1] == "virtio_pci_notify_write":
            events.append((stamp(w[0]), "notify"))
        elif w[1].startswith("virtio_gpu_cmd_"):
            kind = w[1][len("virtio_gpu_cmd_"):]
            events.append((stamp(w[0]), kind))
            if kind == "ctx_submit":
                subs.append(stamp(w[0]))
    print("# submission detaches unrefs notifies detach_only unref_only both empty"
          " notifies_a_detach commands_a_notify longest_unref_run")
    for i in range(1, min(4, len(subs) - 1)):
        lo, hi = subs[i], subs[i + 1]
        d = [t for t, k in events if k == "ctx_res_detach" and lo <= t < hi]
        u = [t for t, k in events if k == "res_unref" and lo <= t < hi]
        if len(d) < 2 or not u:
            continue
        a, b = d[1], max(u)
        burst = [(t, k) for t, k in events if a <= t <= b]
        groups, cur = [], None
        for t, k in burst:
            if k == "notify":
                cur = []
                groups.append(cur)
            elif cur is not None and k in ("ctx_res_detach", "res_unref"):
                cur.append(k)
        nd = sum(1 for _, k in burst if k == "ctx_res_detach")
        nu = sum(1 for _, k in burst if k == "res_unref")
        donly = sum(1 for g in groups if g and all(k == "ctx_res_detach" for k in g))
        uonly = sum(1 for g in groups if g and all(k == "res_unref" for k in g))
        empty = sum(1 for g in groups if not g)
        both = len(groups) - donly - uonly - empty
        carried = sum(len(g) for g in groups)
        run = best = 0
        for g in groups:
            for k in g:
                run = run + 1 if k == "res_unref" else 0
                best = max(best, run)
            run = 0
        print("%d %d %d %d %d %d %d %d %.2f %.2f %d" % (
            i + 1, nd, nu, len(groups), donly, uonly, both, empty,
            float(len(groups)) / nd, float(carried) / max(1, len(groups)), best))


if __name__ == "__main__":
    main()
