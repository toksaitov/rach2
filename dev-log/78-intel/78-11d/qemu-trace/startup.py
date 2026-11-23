"""Account for the first WebGPU command batch on the GPU's main thread."""
from collections import Counter
import gzip
import json
from pathlib import Path
import re
import sys

from read import HOLD, RUN, SELECT, kernel

CALL = 5
FILE = {0: "read", 3: "close", 5: "fstat", 8: "lseek", 17: "pread64",
        83: "mkdir", 87: "unlink", 257: "openat", 258: "mkdirat",
        262: "newfstatat", 263: "unlinkat"}
TRACE_LINE = re.compile(r" (\d+)\.(\d{6}): (\w+): (.*)$")


def batch(case):
    """Find the first 1,704-byte batch after the page's first callback."""
    with gzip.open(case / "out/trace.json.gz", "rt") as trace:
        events = json.load(trace)["traceEvents"]
    first, = (event["ts"] for event in events
              if event["name"] == "hasvk-frame-0" and event["ph"] == "I")
    item, = (event for event in events if event["ph"] == "X"
             and event["name"] == "WebGPUDecoderImpl::HandleDawnCommands"
             and event.get("args", {}).get("bytes") == 1704
             and first <= event.get("ts", -1) <= first + 15000)
    return item


def overlap(a, b, lo, hi):
    """Length of the intersection of two intervals."""
    return max(0, min(b, hi) - max(a, lo))


def off_cpu(edges, lo, hi):
    """Pair selections away with the next resumption, clipped to the batch."""
    start = None
    spans = []
    for t, running in edges:
        if running and start is None:
            continue
        if running:
            if overlap(start, t, lo, hi):
                spans.append((max(start, lo), min(t, hi)))
            start = None
        else:
            if start is not None and overlap(start, t, lo, hi):
                raise ValueError("missing resumption in the batch")
            start = t
    if start is not None and start < hi:
        spans.append((max(start, lo), hi))
    return spans


def rach2_rows(case, item, tid):
    """Read thread trace intervals and convert TSC cycles to monotonic microseconds."""
    rows, convert, rate, begin, end, _ = kernel(case / "kernel.bin")
    lo = convert(item["ts"])
    hi = convert(item["ts"] + item["dur"])
    if not begin <= lo < hi <= end:
        raise ValueError("batch lies outside the kernel trace")
    edges, calls, holds = [], [], []
    for t, start, waited, who, nr, arg, vector, _, kind in rows:
        if kind == SELECT and arg == tid:
            edges.append((t, False))
        elif who == tid:
            if kind == RUN:
                edges.append((t, True))
            elif kind == CALL and overlap(start, t, lo, hi):
                calls.append((start, t, nr))
            elif kind == HOLD and overlap(start - waited, t, lo, hi):
                holds.append((start, t, waited, nr, vector))
    if (case / "gpu-tid").exists() and not calls:
        raise ValueError("filtered trace contains no calls in the batch")
    return lo, hi, rate, edges, calls, holds


def linux_rows(case, item, tid):
    """Read the tracefs instance's syscall pairs and switches on its mono clock."""
    lo, hi = item["ts"], item["ts"] + item["dur"]
    edges, calls = [], []
    marks = {}
    entry = None
    for line in (case / "linux-calls").read_text().splitlines():
        counts = re.search(r"entries-in-buffer/entries-written: (\d+)/(\d+)", line)
        if counts and counts[1] != counts[2]:
            raise ValueError("Linux trace lost records")
        match = TRACE_LINE.search(line)
        if not match:
            continue
        sec, frac, event, fields = match.groups()
        t = int(sec) * 1000000 + int(frac)
        if event == "sys_enter":
            nr = int(re.search(r"\bNR (-?\d+)", fields)[1])
            if entry and lo <= t <= hi:
                raise ValueError("missing syscall exit in the batch")
            entry = t, nr
        elif event == "sys_exit" and entry:
            start, nr = entry
            if nr != int(re.search(r"\bNR (-?\d+)", fields)[1]) and overlap(start, t, lo, hi):
                raise ValueError("mismatched syscall edges in the batch")
            calls.append((start, t, nr))
            entry = None
        elif event == "sched_switch":
            running = int(re.search(r"\bnext_pid=(\d+)", fields)[1]) == tid
            edges.append((t, running))
        elif event == "tracing_mark_write":
            marks[fields.strip()] = t
    if not marks["webgpu-start"] <= lo < hi <= marks["webgpu-stop"]:
        raise ValueError("batch lies outside the Linux trace")
    if not any(overlap(a, b, lo, hi) for a, b, _ in calls):
        raise ValueError("Linux trace contains no calls in the batch")
    return lo, hi, 1, edges, calls, []


def report(case):
    """Report call wall time, on-CPU time, and residual work in milliseconds."""
    item = batch(case)
    tidfile = case / "gpu-tid"
    if tidfile.is_file():
        tid = int(tidfile.read_text())
    else:
        identities = json.loads((case / "out/tids.json").read_text())
        tid, = (row["tid"] for row in identities if row["tgid"] == item["pid"]
                and item["tid"] in (row["tid"], row["nspid"]))
    print("%s: batch %.3f ms wall, %.3f ms thread clock" %
          (case, item["dur"] / 1000, item["tdur"] / 1000))
    if (case / "kernel.bin").is_file():
        measured = rach2_rows(case, item, tid)
    elif (case / "linux-calls").is_file():
        measured = linux_rows(case, item, tid)
    else:
        return
    lo, hi, rate, edges, calls, holds = measured
    spans = off_cpu(edges, lo, hi)
    off = sum(b - a for a, b in spans)

    def duration(a, b):
        return overlap(a, b, lo, hi)

    def scheduled(a, b):
        return duration(a, b) - sum(overlap(a, b, x, y) for x, y in spans)

    scale = rate / 1000
    print("  scheduler off %.3f ms" % (off * scale))
    if holds:
        held = Counter()
        waited = 0
        for start, end, wait, nr, vector in holds:
            held[nr, vector] += duration(start, end)
            waited += scheduled(start - wait, start)
        total = sum(held.values())
        files = sum(cycles for (nr, vector), cycles in held.items() if vector == 128 and nr in FILE)
        print("  lock held %.3f ms (file calls %.3f), waited %.3f ms while scheduled" %
              (total * scale, files * scale, waited * scale))
        if not calls:
            print("  scheduled outside lock holds and waits %.3f ms" %
                  ((hi - lo - off - total - waited) * scale))
    if calls:
        wall, active, counts = Counter(), Counter(), Counter()
        for a, b, nr in calls:
            if duration(a, b) or (a == b and lo <= a < hi):
                wall[nr] += duration(a, b)
                active[nr] += scheduled(a, b)
                counts[nr] += 1
        files = sum(active[nr] for nr in FILE)
        total = sum(active.values())
        print("  scheduled in file calls %.3f ms, other syscalls %.3f ms,"
              " outside syscalls %.3f ms" %
              (files * scale, (total - files) * scale, (hi - lo - off - total) * scale))
        for nr, cycles in active.most_common():
            print("    %s: %d calls, %.3f ms wall, %.3f ms scheduled" %
                  (FILE.get(nr, nr), counts[nr], wall[nr] * scale, cycles * scale))


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        report(Path(arg))
