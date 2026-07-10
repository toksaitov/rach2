#!/usr/bin/env python3
"""
check_busybox_suid.py: prove the os-real image keeps busybox unprivileged.

Privilege in the os-real image climbs through /bin/su alone, a small setuid-root
program. busybox must stay unprivileged: a setuid busybox would run every one of
its BB_SUID_REQUIRE and BB_SUID_MAYBE applets as root unless an /etc/busybox.conf
confined it, and that config path trips a busybox use-after-free the kernel's
strict munmap faults (see dev-log/15-privilege). So the safe invariant is simply
that busybox carries no set-id bit, and /bin/su does.

This is easy to regress: staging busybox 04755 to let its su applet work would
silently reopen the whole BB_SUID_REQUIRE surface. The os-real image targets
depend on this check, so such a change fails the build rather than shipping.

Usage: check_busybox_suid.py <manifest>
"""
import sys


def staged(manifest_path, path):
    """The (mode, owner) the manifest stages `path` with, or (None, None) if it
    does not. The entry is `<file|text> <mode> <path> ...`, where <mode> is
    octal permissions with an optional :uid:gid (owner "0:0" when omitted)."""
    for line in open(manifest_path):
        s = line.strip()
        parts = s.split(None, 3)
        if len(parts) >= 3 and parts[0] in ("file", "text") and parts[2] == path:
            bits, _, owner = parts[1].partition(":")
            try:
                return int(bits, 8), (owner or "0:0")
            except ValueError:
                return None, None
    return None, None


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_busybox_suid.py <manifest>")
    manifest_path = sys.argv[1]

    problems = []

    bb, _ = staged(manifest_path, "/bin/busybox")
    if bb is None:
        problems.append("/bin/busybox is not staged by the manifest")
    elif bb & 0o6000:
        problems.append("/bin/busybox is staged setuid/setgid (mode %o); a "
                        "privileged busybox runs its BB_SUID_REQUIRE/MAYBE "
                        "applets as root. Privilege must climb through /bin/su "
                        "only." % bb)

    su, su_owner = staged(manifest_path, "/bin/su")
    if su is None:
        problems.append("/bin/su is not staged; nothing lets a user become root")
    else:
        if not (su & 0o4000):
            problems.append("/bin/su is staged without the setuid bit (mode %o); "
                            "it cannot grant root" % su)
        if su_owner != "0:0":
            problems.append("/bin/su is owned %s, not root; a setuid bit there "
                            "grants that owner, not root" % su_owner)

    if problems:
        sys.stderr.write("check_busybox_suid: FAIL\n")
        for p in problems:
            sys.stderr.write("  - " + p + "\n")
        return 1

    print("check_busybox_suid: OK (busybox unprivileged; /bin/su is the one "
          "setuid path)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
