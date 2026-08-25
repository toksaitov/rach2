#!/usr/bin/env python3
"""Check the configuration built into a GRUB core image.

Two builders write core images for this project: `tools/image/mkdisk_grub.sh` writes
the one a disk ships with, and `tools/image/mkgrubcore.sh` writes the ones `bootctl
grub` installs over it. Both embed `tools/image/grub-embed.cfg`, and what that file
must say is the difference between a machine that can be repaired when its boot
partition is damaged and one that cannot: two `configfile` calls, partition 1
first and partition 3 second.

The order is the whole of it. The first call opens the menu on partition 1 and
never returns, since a menu that boots something does not come back, so a file
naming partition 3 first would send every healthy boot to recovery. The early
parser these run through has no `if`, which is why they are a pair and not a
test.

This is checked on the file, not on the image because `grub-mkimage`
compresses what it embeds, and the text is not there to find afterwards. What
is required is the whole command list and not the presence of two lines in it:
GRUB takes `;` as a command separator, so anything that looked only for the two
expected lines would pass a file whose first command sent every healthy machine
to recovery. A commented-out line reads as an absent one.

Usage: check_grubembed.py <config> | check_grubembed.py --selftest
"""

import sys

WANT = [
    "configfile (hd0,msdos1)/boot/grub/grub.cfg",
    "configfile (hd0,msdos3)/boot/grub/recovery.cfg",
]


def judge(text):
    """The reason this text is not the configuration, or None when it is."""
    commands = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        commands.append(line)
    if commands != WANT:
        return "the commands are %r, not %r" % (commands, WANT)
    return None


CASES = [
    ("the configuration itself", "\n".join(WANT) + "\n", True),
    ("comments and blank lines around it",
     "# a comment\n\n" + "\n".join(WANT) + "\n\n# another\n", True),
    ("partition 3 commented out", WANT[0] + "\n# " + WANT[1] + "\n", False),
    ("partition 3 missing", WANT[0] + "\n", False),
    ("the two the other way round", "\n".join(reversed(WANT)) + "\n", False),
    ("a third configfile", "\n".join(WANT) + "\nconfigfile (hd0,msdos2)/x.cfg\n", False),
    ("a command before them", "echo hello\n" + "\n".join(WANT) + "\n", False),
    ("a command hidden behind a semicolon",
     "echo hello; " + WANT[1] + "\n" + "\n".join(WANT) + "\n", False),
    ("nothing at all", "", False),
]


def selftest():
    bad = 0
    for name, text, want_ok in CASES:
        got_ok = judge(text) is None
        if got_ok != want_ok:
            bad += 1
            print("check-grubembed: selftest: fail: %s" % name, file=sys.stderr)
    if bad:
        return 1
    print("check-grubembed: selftest: ok: %d accepted, %d refused"
          % (sum(1 for c in CASES if c[2]), sum(1 for c in CASES if not c[2])))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 2:
        sys.exit("usage: check_grubembed.py <config> | check_grubembed.py --selftest")
    path = argv[1]
    try:
        with open(path, "r", encoding="ascii") as f:
            text = f.read()
    except OSError as e:
        sys.exit("check-grubembed: cannot read %s: %s" % (path, e))

    why = judge(text)
    if why:
        sys.exit("check-grubembed: %s: %s" % (path, why))
    print("check-grubembed: OK (%s reaches partition 1 and then partition 3)" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
