#!/usr/bin/env python3
"""trace.py: live virtio-gpu resources over time from QEMU's trace, for task
5676's measurement.

    python3 trace.py TRACE [STEP]

TRACE is QEMU's log of the events in `events`, written with -msg
timestamp=on. A resource lives from its RESOURCE_CREATE to its UNREF; its
bytes are a 3D resource's width by height by depth in whole blocks of its
virgl format, by the table of rach2/sys/drm/virtio_gpu_3d.c, a 2D resource's
width by height by 4 and a blob's size. Prints the creates, unrefs and
submissions, the peak of live resources and of their bytes with the time to
each from the first create of a 1024x1024 resource, the test's first, and
then a line each STEP seconds (5) of the time, live resources, live bytes in
MiB and submissions so far.
"""
import datetime
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "..", "..", "..", "rach2", "sys", "drm", "virtio_gpu_3d.c")


def blocks():
    """Each virgl format's (w, h, bytes) by number, from the driver's table."""
    out = {}
    with open(TABLE) as f:
        for line in f:
            m = re.match(r"\s*\{(\d+),\s*(\d+),\s*(\d+)\},\s*/\* (\d+) ", line)
            if m:
                out[int(m.group(4))] = tuple(int(m.group(i)) for i in (1, 2, 3))
    return out


def stamp(s):
    return datetime.datetime.strptime(s.rstrip("Z"), "%Y-%m-%dT%H:%M:%S.%f").timestamp()


def main():
    path = sys.argv[1]
    step = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    fmt = blocks()
    live = {}
    nbytes = 0
    events = []  # (time, live, bytes, submits)
    creates = unrefs = submits = 0
    start = None
    peak = (0, 0.0)
    peak_bytes = (0, 0.0)
    pat = re.compile(r"^(\S+Z) (virtio_gpu_cmd_\w+) (.*)$")
    with open(path, errors="replace") as f:
        for line in f:
            m = pat.match(line.strip())
            if not m:
                continue
            t, ev, args = stamp(m.group(1)), m.group(2), m.group(3)
            kv = dict(re.findall(r"(\w+) (0x[0-9a-f]+|-?\d+)", args))
            val = {k: int(v, 0) for k, v in kv.items()}
            if ev in ("virtio_gpu_cmd_res_create_3d", "virtio_gpu_cmd_res_create_2d",
                      "virtio_gpu_cmd_res_create_blob"):
                creates += 1
                if ev.endswith("blob"):
                    b = val["size"]
                elif ev.endswith("2d"):
                    b = val["w"] * val["h"] * 4
                else:
                    bw, bh, bb = fmt.get(val["fmt"], (1, 1, 4))
                    bb = bb or 4
                    b = (-(-val["w"] // bw)) * (-(-val["h"] // bh)) * max(val["d"], 1) * bb
                    if start is None and val["w"] == 1024 and val["h"] == 1024:
                        start = t
                nbytes += b - live.get(val["res"], 0)
                live[val["res"]] = b
            elif ev == "virtio_gpu_cmd_res_unref":
                unrefs += 1
                nbytes -= live.pop(val["res"], 0)
            elif ev == "virtio_gpu_cmd_ctx_submit":
                submits += 1
            events.append((t, len(live), nbytes, submits))
            if len(live) > peak[0]:
                peak = (len(live), t)
            if nbytes > peak_bytes[0]:
                peak_bytes = (nbytes, t)
    if not events:
        print("trace: no events")
        return 1
    t0 = start if start is not None else events[0][0]
    print("trace: creates %d, unrefs %d, submissions %d" % (creates, unrefs, submits))
    print("trace: peak: %d resources at %.1f s; %.1f MiB at %.1f s; first 1024x1024 create at %s"
          % (peak[0], peak[1] - t0, peak_bytes[0] / 1048576.0, peak_bytes[1] - t0,
             "none" if start is None else "%.1f s of the trace" % (start - events[0][0])))
    print("trace: last event at %.1f s: %d resources, %.1f MiB" % (
        events[-1][0] - t0, events[-1][1], events[-1][2] / 1048576.0))
    print("# s live MiB submissions")
    nxt = t0
    for t, n, b, s in events:
        if t >= nxt:
            print("%.0f %d %.1f %d" % (t - t0, n, b / 1048576.0, s))
            while nxt <= t:
                nxt += step
    return 0


if __name__ == "__main__":
    sys.exit(main())
