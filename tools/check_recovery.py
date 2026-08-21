#!/usr/bin/env python3
"""
check_recovery.py: read a kernel's disk authority off the built artifact.

Milestone 25 seals a disk into a laptop nobody will open again, and splits the
authority to write it in two. Every image may write the boot partition and the
boot loader's gap, both bounded windows. Only a recovery image may write any
sector of the whole device, which is what reflashing a broken root needs and
what an everyday boot has no business being able to do at all.

The split is a build switch (`-DRACH2_RECOVERY`, see rach2/sys/block/recovery.c)
rather than a boot flag, because a flag on the kernel command line is editable
at the GRUB prompt by anyone standing at the machine. A build switch is only as
good as the check that the artifact actually got built with it, and there is
nothing useful to check for by absence: the two kernels differ by which body of
one function was compiled, not by a symbol. So recovery.c has an explicit
banner naming the authority, and this reads it out of the binary and requires
BOTH halves: the expected banner present, and the other one absent. An everyday
kernel offered as recovery fails this, and so does a recovery kernel shipped as
the everyday one, which is the more dangerous direction.

What this catches is a mis-BUILT artifact: a kernel that was compiled without
the switch and offered as recovery, or one compiled with it and shipped as the
everyday kernel. It is not proof against a tampered one. The banner is a string
in .rodata and the write handler is a separate conditional, so editing eight
bytes of a built kernel changes what this reads without changing what the kernel
does. The behavioural proof is `make authority-test`, which boots both kernels
and requires the ordinary one to refuse a write and leave the sector unchanged;
this check exists so a build mistake is caught before that test runs, not
instead of it.

Usage:
    check_recovery.py <kernel> recovery|ordinary
"""
import sys

BANNERS = {
    "recovery": b"rach2: disk authority: recovery",
    "ordinary": b"rach2: disk authority: ordinary",
}


def main(argv):
    if len(argv) != 3 or argv[2] not in BANNERS:
        sys.stderr.write("usage: check_recovery.py <kernel> recovery|ordinary\n")
        return 2
    path, want = argv[1], argv[2]
    other = "ordinary" if want == "recovery" else "recovery"

    try:
        with open(path, "rb") as handle:
            image = handle.read()
    except OSError as error:
        sys.stderr.write("check-recovery: cannot read %s: %s\n" % (path, error))
        return 2
    if not image:
        sys.stderr.write("check-recovery: %s is empty, so this check proved nothing\n" % path)
        return 1

    # The two banners share a prefix, so each is counted whole and the pair is
    # judged together rather than one being tested by itself.
    has_want = BANNERS[want] in image
    has_other = BANNERS[other] in image

    if not has_want:
        sys.stderr.write(
            "check-recovery: %s does not carry the %s banner; it was not built "
            "with the policy this image needs\n" % (path, want))
        return 1
    if has_other:
        sys.stderr.write(
            "check-recovery: %s has the %s banner as well as the %s one, so "
            "which policy it enforces cannot be read off it\n" % (path, other, want))
        return 1

    sys.stderr.write("check-recovery: OK (%s is %s)\n" % (path, want))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
