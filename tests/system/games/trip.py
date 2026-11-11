#!/usr/bin/env python3
"""
Time WineD3D's GL round trip, tripprobe's 1000 Direct3D 11 clears, copies and
maps, N times in run.py's workspace, and read the kernel's counters at the
loop's start and end; then run it once under strace -f -c -w, once with its
ioctls traced, where strace is installed, and once under perf stat where perf
is.
Usage: trip.py OUT N. The counters of each run go to OUT/trip-gl-I.loop.txt
and its summary to the output.
"""
import collections
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import time

import run

# /proc/stat's counters of the big lock and the scheduler that a loop's
# summary lists; Linux has only ctxt of them.
STAT_KEYS = ("ctxt", "bkl_wait_us", "scheduler_picks", "tlb_shootdown_ipis")


def snapshot():
    """/proc/stat and each tripprobe thread's stat and status, with the
    monotonic time, as one text."""
    text = ["time %.6f\n" % time.monotonic(), Path("/proc/stat").read_text()]
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if "trip64" not in Path("/proc/%s/comm" % pid).read_text():
                continue
            for tid in sorted(os.listdir("/proc/%s/task" % pid)):
                base = "/proc/%s/task/%s/" % (pid, tid)
                text.append("== %s %s\n%s%s" % (pid, tid, Path(base + "stat").read_text(),
                                                   Path(base + "status").read_text()))
        except OSError:
            pass
    return "".join(text)


def parse(text):
    """The time, the /proc/stat counters, the big lock's hold lines and each
    thread's ticks and context switches of a snapshot."""
    stat, holds, threads, tid, at = {}, {}, {}, None, 0.0
    for line in text.splitlines():
        f = line.split()
        if line.startswith("time "):
            at = float(f[1])
        elif line.startswith("== "):
            tid = f[2]
            threads[tid] = {}
        elif tid is None and line.startswith("bkl_hold_entry "):
            holds[f[1] + " " + f[2]] = (int(f[3]), int(f[4]))
        elif tid is None and len(f) == 2 and f[1].isdigit():
            stat[f[0]] = int(f[1])
        elif tid is not None and re.match(r"\d+ \(", line):
            rest = line[line.rindex(")") + 2:].split()
            threads[tid]["utime"], threads[tid]["stime"] = int(rest[11]), int(rest[12])
        elif tid is not None:
            m = re.match(r"(Name|voluntary_ctxt_switches|nonvoluntary_ctxt_switches):\s*(\S+)",
                         line)
            if m:
                threads[tid][m.group(1)] = m.group(2)
    return at, stat, holds, threads


def summary(start, end):
    """Lines that give the loop's wall time, the counters' changes, the big
    lock's holds by entry and each thread's switches between two snapshots."""
    t0, s0, h0, a = parse(start)
    t1, s1, h1, b = parse(end)
    lines = ["window %.1f ms" % ((t1 - t0) * 1000)]
    lines += ["%s %d" % (k, s1[k] - s0[k]) for k in STAT_KEYS if k in s0 and k in s1]
    for key in sorted(h1, key=lambda k: -(h1[k][1] - h0.get(k, (0, 0))[1])):
        if key in h0 and h1[key][0] > h0[key][0]:
            n, us = h1[key][0] - h0[key][0], h1[key][1] - h0[key][1]
            lines.append("hold %s: %d holds, %d us, %.1f us each" % (key, n, us, us / n))
    for tid, y in sorted(b.items()):
        x = a.get(tid, {})

        def change(k):
            return int(y.get(k, 0)) - int(x.get(k, 0))
        lines.append("thread %s %s: utime %d stime %d voluntary %d involuntary %d" % (
            tid, y.get("Name", "?"), change("utime"), change("stime"),
            change("voluntary_ctxt_switches"), change("nonvoluntary_ctxt_switches")))
    return lines


class Runner(run.Runner):
    """run.py's workspace, whose probe also snapshots the loop's start, when
    the probe's output has its feature level line, and its end, when it has
    its result."""

    def probe(self, name, extra=None, program=None, renderer=None):
        path = self.work / (name + ".txt")
        stop, shots = threading.Event(), []
        if path.exists():
            path.unlink()

        def watch():
            want = "feature level"
            while not stop.is_set():
                text = path.read_text(errors="replace") if path.exists() else ""
                if want in text:
                    shots.append(snapshot())
                    if want == "round trips":
                        return
                    want = "round trips"
                time.sleep(0.002)

        thread = threading.Thread(target=watch)
        thread.start()
        try:
            status, text = super().probe(name, extra, program, renderer)
        finally:
            stop.set()
            thread.join()
        if len(shots) == 2:
            (self.out / (name + ".loop.txt")).write_text(shots[0] + "\n==== end\n" + shots[1])
            for line in summary(shots[0], shots[1]):
                run.say("%s: %s" % (name, line))
        return status, text


def ioctls(path):
    """The ioctls of an strace -f trace counted by request, most first."""
    count = collections.Counter()
    for line in path.read_text(errors="replace").splitlines():
        m = re.search(r"ioctl\(\d+, ([A-Za-z0-9_]+|0x[0-9a-f]+)", line)
        if m:
            count[m.group(1)] += 1
    return count.most_common()


def main():
    """Run the round trips in OUT, N times plain and once under each tracer."""
    if len(sys.argv) != 3:
        raise SystemExit("usage: trip.py OUT N")
    out, count = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    out.mkdir(parents=True, exist_ok=False)
    work = Path(tempfile.mkdtemp(prefix="rach2-trip.", dir=os.environ.get("TMPDIR", "/tmp")))
    runner = None
    try:
        runner = Runner(out, work, reference=True)
        runner.start_weston()
        runner.make_prefix()
        base = list(runner.launcher)
        modes = [("trip-gl-%d" % i, []) for i in range(count)]
        if shutil.which("strace"):
            modes.append(("trip-gl-strace", ["strace", "-f", "-c", "-w", "-o",
                                             str(out / "strace-c.txt")]))
            modes.append(("trip-gl-ioctl", ["strace", "-f", "-e", "trace=ioctl", "-o",
                                            str(out / "strace-ioctl.txt")]))
        else:
            run.say("trip: no strace")
        if shutil.which("perf"):
            modes.append(("trip-gl-perf", ["perf", "stat", "-e",
                                           "task-clock,context-switches,cpu-migrations,"
                                           "page-faults", "-o", str(out / "perf.txt")]))
        for name, prefix in modes:
            runner.launcher = prefix + base
            status, text = runner.probe(name, {"WINEDEBUG": "-all,err+all"}, program="trip64")
            found = re.search(r"round trips in ([0-9.]+) ms", text)
            run.say("%s: exit %d ms %s" % (name, status, found.group(1) if found else None))
        if (out / "strace-ioctl.txt").exists():
            for request, n in ioctls(out / "strace-ioctl.txt"):
                run.say("ioctl %s %d" % (request, n))
        print("TRIPS OK", flush=True)
        return 0
    finally:
        if runner is not None:
            runner.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
