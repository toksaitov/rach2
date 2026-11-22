#!/usr/bin/env python3
"""bursts.py: the RESOURCE_UNREFs that follow each submission in a QEMU
trace of task 5676's measurement (dev-log/86-shared-region/measure/events).

    python3 bursts.py TRACE

For each virtio_gpu_cmd_ctx_submit, the UNREFs up to the next submission,
grouped into bursts whose UNREFs come less than GAP seconds apart; prints,
for each submission with any, the count, the first burst's start after the
submission, its UNREFs and its length from first to last UNREF, and the
UNREFs and length of the largest burst.
"""
import datetime
import sys

GAP = 0.05


def stamp(line):
    t = datetime.datetime.strptime(line.split()[0], "%Y-%m-%dT%H:%M:%S.%fZ")
    return t.timestamp()


def main():
    subs, unrefs = [], []
    for line in open(sys.argv[1]):
        if " virtio_gpu_cmd_ctx_submit " in line:
            subs.append(stamp(line))
        elif " virtio_gpu_cmd_res_unref " in line:
            unrefs.append(stamp(line))
    print("# submission at_s unrefs first_after_ms first_n first_ms largest_n largest_ms")
    for i, s in enumerate(subs):
        end = subs[i + 1] if i + 1 < len(subs) else float("inf")
        u = [t for t in unrefs if s <= t < end]
        if not u:
            continue
        bursts, cur = [], [u[0]]
        for t in u[1:]:
            if t - cur[-1] < GAP:
                cur.append(t)
            else:
                bursts.append(cur)
                cur = [t]
        bursts.append(cur)
        big = max(bursts, key=len)
        f = bursts[0]
        print("%d %.3f %d %.1f %d %.1f %d %.1f" % (
            i + 1, s - subs[0], len(u), (f[0] - s) * 1e3, len(f), (f[-1] - f[0]) * 1e3,
            len(big), (big[-1] - big[0]) * 1e3))


if __name__ == "__main__":
    main()
