#!/usr/bin/env python3
"""clockload.py: the guest's clocks against the host's while the host stops it.

The clock suite's driver (tests/system/suites/clock.sh). It boots the test
image to the debug shell, runs musl/clockloadmusl, which samples its
CLOCK_MONOTONIC and CLOCK_REALTIME while it keeps the machine busy, and
meanwhile stops the emulator with SIGSTOP and lets it go with
SIGCONT, STALLS times, each stop STOP_S seconds after RUN_S seconds of
running. A stopped emulator is what a loaded host makes of a machine: its
timers fall due while no processor of the machine runs, and the machine
then sees one interrupt for all the ticks that fell due, or, where the
hypervisor delivers them again, all of them in a burst.

The guest brackets host UDP timestamps with guest clock reads before and
again after the stops. It judges the whole error interval against 250 ms;
serial arrival times have no part in that interval. A reference wider than
100 ms is retried or reported unavailable, never accepted as a clock pass.

Usage: clockload.py KERNEL IMAGE LOG [tcg]

With `tcg`, the machine runs under the emulator whatever accelerator the run
chose: the runner's shim reads RACH2_QEMU_ACCEL, and a run outside the
runner gets `-accel tcg` on the command line. The console is copied to LOG.
Exits 0 when both clocks agree, 1 when one does not or the machine fails,
and 2 for a wrong invocation.

RACH2_CLOCKLOAD_DELAY_S, when set, holds each CLOCKLOAD line that many
seconds before the script reads it, as a slow console would. The proof
tests/proofs/clock-delayed.json sets it, since the judgment must not depend
on when a result arrives.
"""

import os
import queue
import signal
import socket
import subprocess
import sys
import threading
import time

STALLS = 30
RUN_S = 0.7
STOP_S = 0.3


def factor():
    """The run's timeout factor, which scales completion bounds only."""
    try:
        return max(1.0, float(os.environ.get("RACH2_TIMEOUT_FACTOR", "1")))
    except ValueError:
        return 1.0


def delay():
    """Seconds each CLOCKLOAD line is held, from RACH2_CLOCKLOAD_DELAY_S."""
    try:
        return max(0.0, float(os.environ.get("RACH2_CLOCKLOAD_DELAY_S", "0")))
    except ValueError:
        return 0.0


def reader(stream, lines, log):
    """Put each line of `stream` on `lines` with the host time it arrived."""
    held = delay()
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
            if held and line.startswith(b"CLOCKLOAD"):
                time.sleep(held)
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


def reference(sock, stop):
    """Answer requests with host timestamps; serial transport is independent."""
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            packet, peer = sock.recvfrom(160)
        except socket.timeout:
            continue
        fields = packet.split()
        if len(fields) != 2 or fields[0] != b"CLOCKREF" or not fields[1].isdigit():
            continue
        mono = time.monotonic_ns()
        real = time.time_ns()
        sock.sendto(b"CLOCKREF %s %d %d" % (fields[1], mono, real), peer)


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
            "-netdev", "user,id=clockref", "-device", "rtl8139,netdev=clockref"]
    if tcg:
        env["RACH2_QEMU_ACCEL"] = "tcg"
        if not env.get("RACH2_QEMU_LEDGER"):
            argv += ["-accel", "tcg"]
    return argv, env


def run(kernel, image, logpath, tcg):
    """Boot, run the load under the stops, and judge; return the exit status."""
    label = "tcg" if tcg else os.environ.get("RACH2_QEMU_ACCEL", "default")
    argv, env = command(kernel, image, tcg)
    lines = queue.Queue()
    stop = threading.Event()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, open(logpath, "wb") as log:
        sock.bind(("127.0.0.1", 0))
        server = threading.Thread(target=reference, args=(sock, stop))
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env)
        server.start()
        thread = threading.Thread(target=reader, args=(proc.stdout, lines, log), daemon=True)
        thread.start()
        try:
            expect(lines, "Type 'help'", 240 * factor())
            proc.stdin.write(b"exec /musl/clockloadmusl %d\n" % sock.getsockname()[1])
            proc.stdin.flush()
            expect(lines, "CLOCKLOAD READY", 60 * factor())
            stopped = stall(proc.pid)
            proc.stdin.write(b"\n")
            proc.stdin.flush()
            expect(lines, "CLOCKLOAD OK", 120 * factor())
            print("clockload: %s: host stops %.3f s; guest judged bracketed references"
                  % (label, stopped))
            print("clockload: %s: ok" % label)
            return 0
        except (RuntimeError, OSError) as e:
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
            stop.set()
            server.join()


def main():
    if len(sys.argv) not in (4, 5) or (len(sys.argv) == 5 and sys.argv[4] != "tcg"):
        print("usage: clockload.py KERNEL IMAGE LOG [tcg]", file=sys.stderr)
        return 2
    return run(sys.argv[1], sys.argv[2], sys.argv[3], len(sys.argv) == 5)


if __name__ == "__main__":
    sys.exit(main())
