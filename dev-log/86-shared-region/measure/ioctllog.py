#!/usr/bin/env python3
"""ioctllog.py: what a log of ioctlcount.c says about one run of
streaming-texture-leak, for task 5676's measurement.

    python3 ioctllog.py CALLS

CALLS holds four unsigned 64-bit words a call: its start in nanoseconds of
CLOCK_MONOTONIC, its duration, its number and its result. Prints, by kind,
the calls, their total and longest time and their errors, then for each
EXECBUFFER after the first the flush it ends: the GEM_CLOSEs from it to the
next RESOURCE_CREATE, their wall time and their own, the gap to that create,
and the WAITs and creates until the next EXECBUFFER.
"""
import struct
import sys

NAMES = {0x09: "GEM_CLOSE", 0x42: "EXECBUFFER", 0x44: "RESOURCE_CREATE", 0x46: "TRANSFER_FROM",
         0x47: "TRANSFER_TO", 0x48: "WAIT", 0x4a: "CREATE_BLOB"}


def main():
    data = open(sys.argv[1], "rb").read()
    calls = sorted(struct.unpack_from("<QQQq", data, i) for i in range(0, len(data) - 31, 32))
    calls = [c for c in calls if c[0]]
    if not calls:
        print("ioctllog: no calls")
        return 1
    t0 = calls[0][0]
    kinds = {}
    for t, d, nr, rc in calls:
        k = kinds.setdefault(nr, [0, 0, 0, {}])
        k[0] += 1
        k[1] += d
        k[2] = max(k[2], d)
        if rc < 0:
            k[3][rc] = k[3].get(rc, 0) + 1
    print("ioctllog: %d calls over %.3f s" % (len(calls), (calls[-1][0] - t0) / 1e9))
    for nr in sorted(kinds):
        n, total, most, errs = kinds[nr]
        print("ioctllog: %-15s %6d calls, %8.3f s in all, %7.3f ms each, %7.3f ms the longest%s" % (
            NAMES.get(nr, hex(nr)), n, total / 1e9, total / n / 1e6, most / 1e6,
            "".join(", %d answered %d" % (v, e) for e, v in sorted(errs.items()))))
    execs = [i for i, c in enumerate(calls) if c[2] == 0x42]
    print("# flush at s: closes, their wall s, their own s, longest close ms; gap to next create s;"
          " then waits (busy) and creates to the next flush")
    for j, i in enumerate(execs):
        end = execs[j + 1] if j + 1 < len(execs) else len(calls)
        k = i + 1
        closes = []
        while k < end and calls[k][2] != 0x44:
            if calls[k][2] == 0x09:
                closes.append(calls[k])
            k += 1
        waits = [c for c in calls[k:end] if c[2] == 0x48]
        creates = [c for c in calls[k:end] if c[2] == 0x44]
        t = calls[i][0]
        if closes:
            wall = (closes[-1][0] + closes[-1][1] - closes[0][0]) / 1e9
            own = sum(c[1] for c in closes) / 1e9
            most = max(c[1] for c in closes) / 1e6
            last = closes[-1][0] + closes[-1][1]
        else:
            wall = own = most = 0.0
            last = calls[i][0] + calls[i][1]
        gap = (calls[k][0] - last) / 1e9 if k < end else float("nan")
        print("%.3f: %d closes, %.3f s wall, %.3f s own, %.3f ms longest; %.3f s to the next create;"
              " %d waits (%d busy), %d creates" % (
                  (t - t0) / 1e9, len(closes), wall, own, most, gap, len(waits),
                  sum(1 for c in waits if c[3] == -16), len(creates)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
