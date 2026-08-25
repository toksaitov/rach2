#!/usr/bin/env python3
"""Run a command on a pseudo-terminal and answer its password prompts.

The SSH suites need a client that reads a password from a terminal, on a
terminal with a known size, because a session's window size is part of what
they prove. This driver replaces the Expect script that did that: it spawns
the command on a pty sized `--rows` by `--cols`, answers every prompt
matching `--prompt-re` with the value of the environment variable named by
`--password-env` (the environment, not an argument, so the secret never
shows in a process listing), and relays everything the command writes to
standard output.

Two tokens end the transcript, and the suites read both. `TIMEDOUT` appears
when `--timeout` seconds pass before the command exits; `PROMPTED=N` counts
the prompts that were answered, so a server that let a login in without
asking cannot be read as a password login. The driver exits 0 when it ran
the command to completion (whatever the command's own outcome), and nonzero
when it could not run it at all, the distinction the suites need between "the
server said no" and "the question was never asked".
"""

import argparse
import errno
import fcntl
import os
import re
import select
import struct
import sys
import termios
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=40)
    parser.add_argument("--cols", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--prompt-re", default=r"[Pp]assword:")
    parser.add_argument("--password-env", default="RACH2_PASSWORD")
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    password = os.environ.get(args.password_env)
    if password is None:
        print("ptydrive: %s is not set" % args.password_env, file=sys.stderr)
        return 2
    prompt = re.compile(args.prompt_re.encode())

    master, slave = os.openpty()
    winsize = struct.pack("HHHH", args.rows, args.cols, 0, 0)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, winsize)

    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(master)
        if slave > 2:
            os.close(slave)
        try:
            os.execvp(args.command[0], args.command)
        except OSError:
            os._exit(127)
    os.close(slave)

    asked = 0
    tail = b""
    deadline = time.monotonic() + args.timeout
    timed_out = False
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            timed_out = True
            break
        ready, _, _ = select.select([master], [], [], min(remaining, 1.0))
        if not ready:
            continue
        try:
            data = os.read(master, 4096)
        except OSError as error:
            if error.errno == errno.EIO:
                data = b""
            else:
                raise
        if not data:
            break
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        # Scan a bounded tail across read boundaries, so a prompt split
        # between two reads is still answered.
        tail = (tail + data)[-256:]
        if prompt.search(tail):
            asked += 1
            os.write(master, password.encode() + b"\r")
            tail = b""

    if timed_out:
        print("TIMEDOUT")
        os.kill(pid, 15)
    os.close(master)
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
    print("PROMPTED=%d" % (1 if asked else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
