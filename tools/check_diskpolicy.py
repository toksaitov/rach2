#!/usr/bin/env python3
"""
check_diskpolicy.py: read a kernel's disk policy off the built artifact.

Milestone 24 carries Rach2 to a laptop whose SSD holds somebody's Windows, and
until its owner has approved a carved partition after a full backup, nothing
this kernel does may write that disk. The rule is enforced by a build switch
(`-DRACH2_DISK_READONLY`, see rach2/sys/block/diskcmd.c), and a build switch is
only as good as the check that the artifact actually got built with it.

There is nothing useful to check for by absence. The commands are numbers, and
the functions that issue them are static, so a read-only kernel and a
read-write one differ by a comparison rather than by a symbol or a string that
happens to name a write. So diskcmd.c carries an explicit banner naming the
policy, the kernel prints it at boot, and this reads it out of the binary and
requires BOTH halves: the expected banner present, and the other one absent. A
kernel built without the switch fails this, which is the case worth catching,
and so does one that somehow carries both.

Usage:
    check_diskpolicy.py <kernel> read-only|read-write
"""
import sys

BANNERS = {
    "read-only": b"rach2: disk policy: read-only",
    "read-write": b"rach2: disk policy: read-write",
}


def main(argv):
    if len(argv) != 3 or argv[2] not in BANNERS:
        sys.stderr.write("usage: check_diskpolicy.py <kernel> read-only|read-write\n")
        return 2
    path, want = argv[1], argv[2]
    other = "read-write" if want == "read-only" else "read-only"

    try:
        with open(path, "rb") as handle:
            image = handle.read()
    except OSError as error:
        sys.stderr.write("check-diskpolicy: cannot read %s: %s\n" % (path, error))
        return 2
    if not image:
        sys.stderr.write("check-diskpolicy: %s is empty, so this check proved nothing\n" % path)
        return 1

    # The read-write banner is a substring of nothing, but the two banners share
    # a prefix, so each is counted whole and the pair is judged together.
    has_want = BANNERS[want] in image
    has_other = BANNERS[other] in image

    if not has_want:
        sys.stderr.write(
            "check-diskpolicy: %s does not carry the %s banner; it was not built "
            "with the policy this image needs\n" % (path, want))
        return 1
    if has_other:
        sys.stderr.write(
            "check-diskpolicy: %s carries the %s banner as well as the %s one, so "
            "which policy it enforces cannot be read off it\n" % (path, other, want))
        return 1

    sys.stderr.write("check-diskpolicy: OK (%s is %s)\n" % (path, want))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
