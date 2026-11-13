#!/usr/bin/env python3
"""
Time WineD3D's GL round trip, tripprobe's 1000 Direct3D 11 clears, copies and
maps, N times in run.py's workspace, and read the kernel's counters at the
loop's start and end; then run it once under strace -f -c -w, once with its
ioctls traced, where strace is installed, and once under perf stat where perf
is.
Usage: trip.py OUT N. The counters of each run go to OUT/trip-gl-I.loop.txt
and its summary to the output.

Rach2's debugfs is root's alone, so the user that runs the loop asks a reader
of root's for each snapshot of bkl/calls, bkl/profile and bkl/tick, and reads
only those copies. trip.sh makes the reader's workspace with
trip.py --prepare UID, which prints its path DIR, and starts the reader with
trip.py --reader DIR. The user appends each request's name, a line, to
DIR/ask, a file of its own, and the reader copies the three files into
DIR/snap/NAME, which the user may read but not write. Every directory there
is root's and writable by root alone, and the reader writes only relative to
directories it holds open, creating each entry with its final mode and
following no symbolic link, so that the user can redirect none of root's
writes.
"""
import collections
import itertools
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import sys
import tempfile
import threading
import time

import run

# /proc/stat's counters of the big lock and the scheduler that a loop's
# summary lists; Linux has only ctxt of them.
STAT_KEYS = ("ctxt", "bkl_wait_us", "scheduler_picks", "tlb_shootdown_ipis")

# debugfs's files of the big lock, Rach2's alone (debugfs(5)), which the
# reader copies and nothing else: calls, the complete holds by entry and the
# takes after a switch, by the system call the thread slept in, and each
# call's and ioctl request's time with rach2.cycleaccount; profile, whose
# samples by entry over the loop the summary gives, and whose whole-kernel
# mode, which root chooses by writing "kernel" to it before the run, samples
# every tick in kernel mode, the lock held or not; and tick, the time of the
# timer tick's parts.
BKL = Path("/sys/kernel/debug/bkl")
BKL_FILES = ("calls", "profile", "tick")

# The reader's directory and process ID, which trip.sh passes; empty where
# there is no bkl directory, as on Linux.
READER_DIR = os.environ.get("TRIP_BKL", "")
READER_PID = os.environ.get("TRIP_BKL_PID", "")
requests = itertools.count(1)


# The reader's workspace's parent, root's and sticky or writable by root
# alone, where a user can rename and remove none of root's entries; never
# TMPDIR, which the caller chooses.
WORKSPACE_PARENT = "/tmp"

# Open flags of a directory the reader writes in: a symbolic link in its
# place fails with ELOOP.
DIR_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC

# Open flags of a file the reader makes: an entry of any kind already there,
# a symbolic link among them, fails with EEXIST.
NEW_FLAGS = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC


def protected(st, what):
    """Refuse the directory whose status is ST unless root owns it and no
    group or other may write it; WHAT names it in the error."""
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != 0 or st.st_mode & 0o022:
        raise SystemExit("trip: %s is not a directory of root's that root alone may write"
                         % what)


def check_parent():
    """Refuse WORKSPACE_PARENT unless it is a directory, not a link, of
    root's that is sticky or writable by root alone."""
    st = os.lstat(WORKSPACE_PARENT)
    if (not stat.S_ISDIR(st.st_mode) or st.st_uid != 0
            or (st.st_mode & 0o022 and not st.st_mode & stat.S_ISVTX)):
        raise SystemExit("trip: %s is not root's, or others may rename its entries"
                         % WORKSPACE_PARENT)


def prepare(uid):
    """As root, make the reader's workspace in WORKSPACE_PARENT and return its
    path: the directory, made 0700 by mkdtemp, with snap, 0755, and ask, an
    empty file of UID's, 0600, made in it through its descriptor while no
    one else may enter it; then the directory made 0755. Removes the
    workspace if any step fails."""
    check_parent()
    os.umask(0o022)
    path = tempfile.mkdtemp(prefix="trip-bkl.", dir=WORKSPACE_PARENT)
    try:
        base = os.open(path, DIR_FLAGS)
        try:
            os.mkdir("snap", 0o755, dir_fd=base)
            fd = os.open("ask", NEW_FLAGS, 0o600, dir_fd=base)
            try:
                os.fchown(fd, uid, -1)
            finally:
                os.close(fd)
            os.fchmod(base, 0o755)
        finally:
            os.close(base)
    except BaseException:
        shutil.rmtree(path, ignore_errors=True)
        raise
    return path


def copy(snap, name):
    """Copy the files of BKL_FILES that read into snap/NAME.tmp, a new
    directory 0755, as files 0644 made through its descriptor, and rename it
    to snap/NAME. SNAP is the descriptor of the snapshots' directory."""
    tmp = name + ".tmp"
    os.mkdir(tmp, 0o755, dir_fd=snap)
    fd = os.open(tmp, DIR_FLAGS, dir_fd=snap)
    try:
        for f in BKL_FILES:
            try:
                text = (BKL / f).read_bytes()
            except OSError:
                continue
            out = os.open(f, NEW_FLAGS, 0o644, dir_fd=fd)
            try:
                while text:
                    text = text[os.write(out, text):]
            finally:
                os.close(out)
    finally:
        os.close(fd)
    os.rename(tmp, name, src_dir_fd=snap, dst_dir_fd=snap)


def serve(path):
    """As root, answer each request named in PATH/ask, a line of at most 16
    digits, once, with a copy of BKL_FILES in PATH/snap/NAME (copy). Opens
    the workspace and snap once, refusing either unless it is a directory of
    root's that root alone may write, and polls ask every millisecond;
    returns when its parent, trip.sh, has gone."""
    check_parent()
    os.umask(0o022)
    base = os.open(path, DIR_FLAGS)
    protected(os.fstat(base), path)
    snap = os.open("snap", DIR_FLAGS, dir_fd=base)
    protected(os.fstat(snap), path + "/snap")
    ask = os.open("ask", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=base)
    if not stat.S_ISREG(os.fstat(ask).st_mode):
        raise SystemExit("trip: %s/ask is not a file" % path)
    parent, done, pending = os.getppid(), set(), b""
    while os.getppid() == parent:
        pending += os.read(ask, 4096)
        lines = pending.split(b"\n")
        pending = lines.pop()
        if len(pending) > 16:
            pending = b""
        for line in lines:
            name = line.decode("ascii", "replace")
            if name in done or not name.isdigit() or len(name) > 16:
                continue
            done.add(name)
            copy(snap, name)
        time.sleep(0.001)


def reader_alive(pid):
    """Whether process PID runs and is not a zombie; a reader that has gone
    stays a zombie until trip.sh, waiting for the loop, reaps it."""
    try:
        stat = Path("/proc/%d/stat" % pid).read_text()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2:].split()[0] not in ("Z", "X")


def bkl_snapshot():
    """The reader's copies of BKL_FILES, by name, taken now; none without a
    reader. Waits for the copy as long as the reader runs."""
    if not READER_DIR:
        return {}
    base, name = Path(READER_DIR), str(next(requests))
    fd = os.open(base / "ask", os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW)
    try:
        os.write(fd, (name + "\n").encode())
    finally:
        os.close(fd)
    path = base / "snap" / name
    while not path.exists():
        if not reader_alive(int(READER_PID)):
            raise RuntimeError("trip: the reader of debugfs has gone")
        time.sleep(0.001)
    return {f: (path / f).read_text() for f in BKL_FILES if (path / f).exists()}


def snapshot():
    """/proc/stat, all entry, slept and ioctl lines of debugfs's bkl/calls,
    its profile's lines, each after "profile ", and bkl/tick's, each after
    "tick ", where the reader read them, and each tripprobe thread's stat
    and status, with the monotonic time, as one text."""
    text = ["time %.6f\n" % time.monotonic(), Path("/proc/stat").read_text()]
    bkl = bkl_snapshot()
    if "calls" in bkl:
        text += ["bkl_calls_complete\n"]
        text += [line + "\n" for line in bkl["calls"].splitlines()
                 if line.startswith(("syscall ", "vector ", "take ", "slept ", "ioctl "))]
    text += ["profile " + line + "\n" for line in bkl.get("profile", "").splitlines()]
    text += ["tick " + line + "\n" for line in bkl.get("tick", "").splitlines()]
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
                text.append("schedstat " + Path(base + "schedstat").read_text())
        except OSError:
            pass
    # The lines of the thread bkl/calls counts alone, which trip.sh -t
    # chooses: its switches away by the thread switched to, the switches to
    # threads past the kernel's 32 slots, and its sleeps.
    text += ["filter " + line + "\n" for line in bkl.get("calls", "").splitlines()
             if line.startswith(("thread ", "switch ", "switch_lost ", "block "))]
    return "".join(text)


def parse(text):
    """The time, the /proc/stat counters, the big lock's hold lines and each
    thread's ticks and context switches of a snapshot."""
    stat, holds, threads, tid, at = {}, {}, {}, None, 0.0
    complete = False
    for line in text.splitlines():
        f = line.split()
        if line.startswith("time "):
            at = float(f[1])
        elif line.startswith("== "):
            tid = f[2]
            threads[tid] = {}
        elif tid is None and line.startswith("bkl_hold_entry "):
            holds[f[1] + " " + f[2]] = (int(f[3]), int(f[4]))
        elif line == "bkl_calls_complete":
            complete = True
            holds.clear()
        elif tid is None and complete and len(f) >= 6 and f[0] in (
                "syscall", "vector", "take", "slept"):
            holds[f[0] + " " + f[1]] = (int(f[4]), int(f[5]))
        elif tid is None and len(f) == 2 and f[1].isdigit():
            stat[f[0]] = int(f[1])
        elif tid is not None and line.startswith("schedstat "):
            threads[tid]["run_ns"], threads[tid]["delay_ns"] = int(f[1]), int(f[2])
        elif line.startswith("filter "):
            continue
        elif tid is not None and re.match(r"\d+ \(", line):
            rest = line[line.rindex(")") + 2:].split()
            threads[tid]["utime"], threads[tid]["stime"] = int(rest[11]), int(rest[12])
        elif tid is not None:
            m = re.match(r"(Name|voluntary_ctxt_switches|nonvoluntary_ctxt_switches):\s*(\S+)",
                         line)
            if m:
                threads[tid][m.group(1)] = m.group(2)
    return at, stat, holds, threads, complete


def entries(text):
    """Each bkl/calls entry's and ioctl request's calls and wall_us, and the
    profile's samples summed by entry, of a snapshot."""
    walls, samples = {}, collections.Counter()
    for line in text.splitlines():
        if line.startswith("== "):
            break
        f = line.split()
        if len(f) >= 6 and f[0] in ("syscall", "ioctl"):
            walls[f[0] + " " + f[1]] = (int(f[2]), int(f[3]))
        elif len(f) >= 4 and f[0] == "profile" and f[-1].isdigit():
            samples[f[1] + " " + f[2]] += int(f[-1])
    return walls, samples


def summary(start, end):
    """Lines that give the loop's wall time, the counters' changes, the big
    lock's holds by entry and each thread's switches between two snapshots."""
    t0, s0, h0, a, c0 = parse(start)
    t1, s1, h1, b, c1 = parse(end)
    lines = ["window %.1f ms" % ((t1 - t0) * 1000)]
    lines += ["%s %d" % (k, s1[k] - s0[k]) for k in STAT_KEYS if k in s0 and k in s1]
    # The kernel prints a slept row only after its first hold, so a row first
    # seen during the loop starts at zero.
    for key in sorted(h1, key=lambda k: -(h1[k][1] - h0.get(k, (0, 0))[1])):
        if key not in h0 and not (c0 and c1):
            continue
        before = h0.get(key, (0, 0))
        if h1[key][0] > before[0]:
            n, us = h1[key][0] - before[0], h1[key][1] - before[1]
            lines.append("hold %s: %d holds, %d us, %.1f us each" % (key, n, us, us / n))
    w0, p0 = entries(start)
    w1, p1 = entries(end)
    spent = {k: (w1[k][0] - w0.get(k, (0, 0))[0], w1[k][1] - w0.get(k, (0, 0))[1]) for k in w1}
    for key in sorted(spent, key=lambda k: -spent[k][1])[:16]:
        n, us = spent[key]
        if n > 0 and us > 0:
            lines.append("call %s: %d calls, %d us, %.1f us each" % (key, n, us, us / n))
    taken = p1 - p0
    for key, n in taken.most_common(16):
        lines.append("profile %s: %d samples" % (key, n))
    for tid, y in sorted(b.items()):
        x = a.get(tid, {})

        def change(k):
            return int(y.get(k, 0)) - int(x.get(k, 0))
        lines.append("thread %s %s: utime %d stime %d voluntary %d involuntary %d" % (
            tid, y.get("Name", "?"), change("utime"), change("stime"),
            change("voluntary_ctxt_switches"), change("nonvoluntary_ctxt_switches")))
        if "run_ns" in y:
            lines.append("thread %s %s: ran %.1f ms, waited ready %.1f ms" % (
                tid, y.get("Name", "?"), change("run_ns") / 1e6, change("delay_ns") / 1e6))
    lines += filtered(start, end)
    return lines


# The key of filtered's line of the switches lost to full slots.
LOST = "switches to threads past the slots"


def filtered(start, end):
    """Lines that give the changes of the chosen thread's switches away, by
    the thread switched to, and of its sleeps, by call and ioctl request,
    and the switches the kernel counted under no thread, once its 32 slots
    were taken; the slots last until bkl/calls is written, so a run's
    switches can be lost to threads of earlier runs, and the line is given
    whenever the counts have it."""
    def counts(text):
        found, name = {}, None
        for line in text.splitlines():
            f = line.split()
            if line.startswith("filter thread "):
                name = f[2]
            elif line.startswith("filter switch ") and len(f) >= 5:
                found["switch to %s %s" % (f[2], " ".join(f[5:]))] = (int(f[3]), int(f[4]))
            elif line.startswith("filter switch_lost ") and len(f) == 3:
                found[LOST] = (int(f[2]), 0)
            elif line.startswith("filter block ") and len(f) == 5:
                found["sleep in %s %s" % (f[2], f[3])] = (int(f[4]), 0)
        return name, found
    name, a = counts(start)
    _, b = counts(end)
    if name is None:
        return []
    lines = ["filter thread %s" % name]
    for key in sorted(b, key=lambda k: -sum(b[k]) + sum(a.get(k, (0, 0)))):
        x, y = a.get(key, (0, 0)), b[key]
        if key.startswith("switch") and y != x:
            lines.append("filter %s: %d yields, %d preemptions" % (key, y[0] - x[0], y[1] - x[1]))
        elif y[0] != x[0] or key == LOST:
            lines.append("filter %s: %d" % (key, y[0] - x[0]))
    return lines


class Runner(run.Runner):
    """run.py's workspace, whose probe also snapshots the loop's start, once
    the probe's output has its feature level line and before it has its
    result, and the loop's end, once it has its result and before the probe
    returns. A probe without both snapshots fails, since its window would
    not be the loop's."""

    def probe(self, name, extra=None, program=None, renderer=None):
        path = self.work / (name + ".txt")
        stop, shots = threading.Event(), []
        if path.exists():
            path.unlink()

        def read():
            return path.read_text(errors="replace") if path.exists() else ""

        def watch():
            while not stop.is_set():
                text = read()
                if not shots:
                    if "round trips" in text:
                        return  # the loop ended before its start was seen
                    if "feature level" in text:
                        shot = snapshot()
                        if "round trips" in read():
                            return  # the loop ended while the snapshot was read
                        shots.append(shot)
                elif "round trips" in text:
                    shot = snapshot()
                    if not stop.is_set():
                        shots.append(shot)
                    return
                time.sleep(0.002)

        thread = threading.Thread(target=watch)
        thread.start()
        try:
            status, text = super().probe(name, extra, program, renderer)
        finally:
            stop.set()
            thread.join()
        if len(shots) != 2:
            raise RuntimeError("%s: no snapshot of the loop's %s" % (
                name, "end" if shots else "start"))
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
    """Run the round trips in OUT, N times plain and once under each tracer;
    or, as root, with --prepare UID make the reader's workspace and print
    its path, and with --reader DIR serve the snapshots of debugfs."""
    if len(sys.argv) == 3 and sys.argv[1] == "--prepare":
        print(prepare(int(sys.argv[2])), flush=True)
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "--reader":
        serve(sys.argv[2])
        return 0
    if len(sys.argv) != 3:
        raise SystemExit("usage: trip.py OUT N | trip.py --prepare UID | trip.py --reader DIR")
    out, count = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    if count < 1:
        raise SystemExit("trip: runs must be positive")
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
            if status != 0 or found is None or "TRIP OK" not in text:
                raise RuntimeError("%s failed or returned no round-trip result" % name)
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
    # run.py's handlers, which its own guard installs only when it is the
    # program: they stop the children and unwind through main's cleanup.
    # The reader and --prepare have neither and end at their signal.
    if sys.argv[1:2] not in (["--reader"], ["--prepare"]):
        for stop_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
            signal.signal(stop_signal, run.interrupted)
    sys.exit(main())
