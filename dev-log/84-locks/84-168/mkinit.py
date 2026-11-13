#!/usr/bin/env python3
"""mkinit.py OUT BUSYBOX PROGRAM ARGS...: write a newc initramfs whose /init
mounts /proc, /sys and /dev, runs /PROGRAM with ARGS between two marker
lines, and powers off."""
import os
import sys


def entry(name, mode, data=b"", rdev=(0, 0), ino=[1]):
    ino[0] += 1
    fields = [ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1],
              len(name) + 1, 0]
    head = b"070701" + b"".join(b"%08X" % f for f in fields)
    out = head + name.encode() + b"\0"
    out += b"\0" * ((4 - len(out) % 4) % 4)
    out += data
    out += b"\0" * ((4 - len(data) % 4) % 4)
    return out


def main():
    out, busybox, program = sys.argv[1:4]
    args = " ".join(sys.argv[4:])
    name = os.path.basename(program)
    init = ("#!/bin/busybox sh\n"
            "/bin/busybox mount -t proc proc /proc\n"
            "/bin/busybox mount -t sysfs sys /sys\n"
            "/bin/busybox mount -t devtmpfs dev /dev\n"
            "echo QRUN-START $(/bin/busybox uname -r)\n"
            "/%s %s\n"
            "echo QRUN-END $?\n"
            "/bin/busybox poweroff -f\n" % (name, args)).encode()
    blob = b""
    for d in ("bin", "dev", "proc", "sys"):
        blob += entry(d, 0o040755)
    blob += entry("dev/console", 0o020600, rdev=(5, 1))
    blob += entry("bin/busybox", 0o100755, open(busybox, "rb").read())
    blob += entry(name, 0o100755, open(program, "rb").read())
    blob += entry("init", 0o100755, init)
    blob += entry("TRAILER!!!", 0)
    open(out, "wb").write(blob)


main()
