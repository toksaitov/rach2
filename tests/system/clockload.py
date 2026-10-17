#!/usr/bin/env python3
"""clockload.py: the guest's clocks against the host's while the host stops it.

The clock suite's driver (tests/system/suites/clock.sh). It boots the test
image to the debug shell, runs musl/clockloadmusl, which prints its
CLOCK_MONOTONIC and CLOCK_REALTIME once a second while it keeps the machine
busy, and meanwhile stops the emulator with SIGSTOP and lets it go with
SIGCONT, STALLS times, each stop STOP_S seconds after RUN_S seconds of
running. A stopped emulator is what a loaded host makes of a machine: its
timers fall due while no processor of the machine runs, and the machine
then sees one interrupt for all the ticks that fell due, or, where the
hypervisor delivers them again, all of them in a burst.

The judgment is the guest's time between the first and the last line it
printed against the host's monotonic time between their arrivals, both
lines printed outside the stops: each clock must agree with the host within
BOUND_MS. The bound was measured on the testing host, as clock.sh records.

Usage: clockload.py KERNEL IMAGE LOG [tcg]

With `tcg`, the machine runs under the emulator whatever accelerator the run
chose: the runner's shim reads RACH2_QEMU_ACCEL, and a run outside the
runner gets `-accel tcg` on the command line. The console is copied to LOG.
Exits 0 when both clocks agree, 1 when one does not or the machine fails,
and 2 for a wrong invocation.
"""

import os
import queue
import signal
import subprocess
import sys
import threading
import time

STALLS = 30
RUN_S = 0.7
STOP_S = 0.3
GUEST_S = 45
BOUND_MS = 250


def factor():
    """The run's timeout factor, which scales completion bounds only."""
    try:
        return max(1.0, float(os.environ.get("RACH2_TIMEOUT_FACTOR", "1")))
    except ValueError:
        return 1.0


def reader(stream, lines, log):
    """Put each line of `stream` on `lines` with the host time it arrived."""
    buf = b""
    while True:
        chunk = stream.read1(4096) if hasattr(stream, "read1") else stream.read(1)
        if not chunk:
            break
        log.write(chunk)
        log.flush()
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            lines.put((time.monotonic(), line.decode("latin-1").rstrip("\r")))
    lines.put((time.monotonic(), None))


def expect(lines, token, seconds):
    """The first line holding `token` within `seconds`, as (time, line)."""
    deadline = time.monotonic() + seconds
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            raise RuntimeError("no '%s' within %d s" % (token, seconds))
        try:
            when, line = lines.get(timeout=left)
        except queue.Empty:
            continue
        if line is None:
            raise RuntimeError("the machine ended before '%s'" % token)
        if "CLOCKLOAD FAIL" in line:
            raise RuntimeError("the guest failed: %s" % line)
        if token in line:
            return when, line


def stamp(when, line):
    """(host seconds, monotonic seconds, real seconds) of an AT line."""
    fields = line.split()
    at = fields.index("AT")
    return when, int(fields[at + 1]) / 1e9, int(fields[at + 2]) / 1e9


def stall(pid):
    """Stop and continue the emulator STALLS times; the seconds it was stopped."""
    stopped = 0.0
    for _ in range(STALLS):
        time.sleep(RUN_S)  # interval: the machine runs between two stops
        began = time.monotonic()
        os.kill(pid, signal.SIGSTOP)
        try:
            time.sleep(STOP_S)  # interval: the host holds the machine stopped
        finally:
            os.kill(pid, signal.SIGCONT)
        stopped += time.monotonic() - began
    return stopped


def command(kernel, image, tcg):
    """The emulator's command line and environment."""
    env = dict(os.environ)
    qemu = env.get("RACH2_QEMU", "qemu-system-x86_64")
    argv = [qemu, "-kernel", kernel, "-initrd", image, "-append", "rach2.rescue",
            "-m", "256", "-serial", "stdio", "-display", "none", "-no-reboot",
            "-net", "none"]
    if tcg:
        env["RACH2_QEMU_ACCEL"] = "tcg"
        if not env.get("RACH2_QEMU_LEDGER"):
            argv += ["-accel", "tcg"]
    return argv, env


def judge(first, last, stopped, label):
    """Print the comparison of `first` and `last` and return whether it passes."""
    host = last[0] - first[0]
    mono = last[1] - first[1]
    real = last[2] - first[2]
    off_mono = (mono - host) * 1000.0
    off_real = (real - host) * 1000.0
    print("clockload: %s: host %.3f s, %.3f s of it stopped; monotonic %.3f s (%+.0f ms), "
          "real %.3f s (%+.0f ms); bound %d ms"
          % (label, host, stopped, mono, off_mono, real, off_real, BOUND_MS))
    return abs(off_mono) <= BOUND_MS and abs(off_real) <= BOUND_MS


def run(kernel, image, logpath, tcg):
    """Boot, run the load under the stops, and judge; return the exit status."""
    label = "tcg" if tcg else os.environ.get("RACH2_QEMU_ACCEL", "default")
    argv, env = command(kernel, image, tcg)
    lines = queue.Queue()
    with open(logpath, "wb") as log:
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env)
        thread = threading.Thread(target=reader, args=(proc.stdout, lines, log), daemon=True)
        thread.start()
        try:
            expect(lines, "Type 'help'", 240 * factor())
            proc.stdin.write(b"exec /musl/clockloadmusl %d\n" % GUEST_S)
            proc.stdin.flush()
            first = stamp(*expect(lines, "CLOCKLOAD AT", 60 * factor()))
            stopped = stall(proc.pid)
            last = None
            deadline = time.monotonic() + (GUEST_S + 120) * factor()
            while True:
                when, line = expect(lines, "CLOCKLOAD", deadline - time.monotonic())
                if "CLOCKLOAD OK" in line:
                    break
                last = stamp(when, line)
            if last is None or last[0] - first[0] < STALLS * (RUN_S + STOP_S):
                print("clockload: %s: FAIL: the last line came before the stops ended" % label)
                return 1
            if not judge(first, last, stopped, label):
                print("clockload: %s: FAIL: the guest's clock is off the host's by more "
                      "than %d ms; see %s" % (label, BOUND_MS, logpath))
                return 1
            print("clockload: %s: ok" % label)
            return 0
        except RuntimeError as e:
            print("clockload: %s: FAIL: %s; see %s" % (label, e, logpath))
            return 1
        finally:
            try:
                os.kill(proc.pid, signal.SIGCONT)
            except OSError:
                pass
            proc.kill()
            proc.wait()
            thread.join(5)


def main():
    if len(sys.argv) not in (4, 5) or (len(sys.argv) == 5 and sys.argv[4] != "tcg"):
        print("usage: clockload.py KERNEL IMAGE LOG [tcg]", file=sys.stderr)
        return 2
    return run(sys.argv[1], sys.argv[2], sys.argv[3], len(sys.argv) == 5)


if __name__ == "__main__":
    sys.exit(main())
