#!/usr/bin/env python3
"""old_ports.py: claim the host lock and a block of host ports as the
`ports` mode of an older checkout's runutil does, and exec a command.

Usage: old_ports.py DIR COUNT COMMAND [ARG...]

It takes the host lock that RACH2_HOST_LOCK asks for, `shared` or `alone`,
by the gate DIR/rach2-host.gate and then DIR/rach2-host.lock, and then the
lowest free of COUNT blocks DIR/rach2-ports.N, each a whole-file record lock
tried once every 50 ms, and execs COMMAND with RACH2_PORT_OFFSET set to
N * 1000 and without RACH2_HOST_LOCK, keeping the locks through the exec.
Its first failed try of the host lock prints runutil's line that says the
claim waits, and so does its first pass over the blocks that finds every one
held.
An older tests/run that bisection runs takes the same files so, and
tests/host/etude_ports_hosttest.c checks that etude-ports and this stand-in
keep each other out.
"""

import fcntl
import os
import sys
import time


def take(path, kind, said, waiting):
    """Open PATH, created mode 0666, and lock it with KIND, waiting while
    another process holds a lock that conflicts. The first wait prints
    WAITING unless SAID, a list, is not empty."""
    mask = os.umask(0)
    try:
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    finally:
        os.umask(mask)
    while True:
        try:
            fcntl.lockf(fd, kind | fcntl.LOCK_NB)
            return fd
        except OSError:
            if not said:
                print(waiting, flush=True)
                said.append(waiting)
            time.sleep(0.05)


def main():
    """Claim the locks, then exec the command."""
    if len(sys.argv) < 4:
        sys.exit("old-ports: usage: old_ports.py DIR COUNT COMMAND [ARG...]")
    directory, count, command = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    keep = []
    said = []
    mode = os.environ.pop("RACH2_HOST_LOCK", None)
    if mode is not None:
        if mode == "alone":
            kind, waiting = fcntl.LOCK_EX, "run: host: waiting for the runs in progress to end"
        else:
            kind, waiting = fcntl.LOCK_SH, "run: host: waiting for a run that needs the host alone"
        gate = take(os.path.join(directory, "rach2-host.gate"), kind, said, waiting)
        keep.append(take(os.path.join(directory, "rach2-host.lock"), kind, said, waiting))
        if mode == "alone":
            keep.append(gate)
        else:
            os.close(gate)
    waited = False
    while True:
        for slot in range(count):
            fd = os.open(os.path.join(directory, f"rach2-ports.{slot}"), os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                os.close(fd)
                continue
            for held in keep + [fd]:
                os.set_inheritable(held, True)
            os.environ["RACH2_PORT_OFFSET"] = str(slot * 1000)
            os.execvp(command[0], command)
        if not waited:
            print(f"run: ports: all {count} blocks are held by other runs; waiting", flush=True)
            waited = True
        time.sleep(0.05)


if __name__ == "__main__":
    main()
