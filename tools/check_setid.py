#!/usr/bin/env python3
"""
check_setid.py: the guard on the set-id bits of the staged images.

Privilege in a Rach2 image climbs through a few declared files and nowhere
else: os-real has busybox's set-id build staged as /bin/su, /bin/passwd and
/bin/login, os-simple has this project's own /bin/su and /bin/passwd, the test
image has the suidprobe and sudoprobe fixtures, and the other images have none.
The kernel honors the set-id bits at exec, so a bit staged anywhere else is a
real privilege hole; the bits are declared in the table below before the build
may stage them.

The guard has four halves by now, and they answer different questions about
different artifacts: the manifests, the staged images, the two busybox applet
tables with the two configs they were built from, and the two init binaries.

The manifest sweep parses the manifests the way `apply_line`
(krffs/stageimage.krffs.c) does, byte for byte: the file is read as latin-1 so
a line is a sequence of bytes and not of characters, only a newline ends a line
the way only a newline ends `fgets`, fields split on spaces and tabs, a comment
is a line whose first token begins with '#' (there is no trailing-comment form,
so `file 0755 /x build/x # note` is a directive with an ignored tail), the mode
is the second token of all five mode-bearing verbs, and an omitted owner reads
as 0:0 the way `parse_mode` defaults it. A set-id `file` or `optional-file`
must appear in that manifest's allowlist at the same path, mode, owner and host
source. A set-id `exec-dir` is refused with no allowlist escape, because the
set of paths it privileges is a property of what happened to be built into the
host directory and not of the manifest. A set-id `dir` is refused too: KRFFS
implements no setgid-directory inheritance, so nothing would ever read the bit.
Sticky is not a set-id bit and stays legal. An unknown verb, a malformed mode,
a directive short of its operands, a path staged twice, an over-long line and a
manifest this table does not name are hard errors, because each is one in
`stageimage` and a guard that accepts what the stager refuses is a guard
reading a different file.

The image inventory reads the artifact rather than the text, because the sweep
trusts one thing it cannot check, that this parser agrees with `apply_line`,
and because `exec-dir` expands to a set of files the manifest never names. It
walks the staged node chain the way fsck.krffs does, rebuilds every reserved
node's path from `parent_id`, and compares the nodes carrying 04000 or 02000
against the allowlist the named manifest owns. The manifest is an argument and
not an inference: the allowlist is per manifest and an image carries no record
of which one staged it. Every declared entry must be found, every unprivileged
path must be present without a bit, and a manifest that declares no set-id
entry names one ordinary node instead, so a walk that finds nothing fails the
build instead of reading as a clean image. The walk also requires the node ring
to close on the size the root records, which is what catches a region that ends
on a node boundary and so tiles perfectly over a prefix of the file system.

Usage:
    check_setid.py [MANIFEST ...]
        Sweep those manifests (every manifest in krffs/manifests when none are
        named) against the allowlist.
    check_setid.py --image FILE --manifest PATH [--offset BYTES] [--size BYTES]
        Inventory a staged image against the allowlist PATH owns. The size is
        required with a nonzero offset: the region is a partition inside a
        larger file, and without it both the wrap at the region end and the
        iteration bound are computed off the wrong end.
    check_setid.py --applets TABLE
        Read the staged busybox applet tables and the configs the two builds
        were resolved from, and the two init binaries.
    check_setid.py --selftest
        Run every half over the fixture manifests, images and configs held in
        this script.

Exits non-zero on any finding.
"""
import argparse
import glob
import os
import re
import struct
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST_DIR = os.path.join(REPO, "krffs", "manifests")

S_ISUID = 0o4000
S_ISGID = 0o2000
SETID = S_ISUID | S_ISGID
PERM = 0o7777

# The five mode-bearing verbs of apply_line, the three of them that stage one
# named file and so can carry a declared set-id bit, the three that name a host
# operand, and the fewest tokens each needs: `file`, `optional-file` and
# `exec-dir` reach for a fourth token and refuse the line without it, while
# `text` treats a missing payload as empty.
FILE_VERBS = ("file", "optional-file", "text")
HOST_VERBS = ("file", "optional-file", "exec-dir")
VERBS = ("dir", "text", "file", "optional-file", "exec-dir")
ARITY = {"dir": 3, "text": 3, "file": 4, "optional-file": 4, "exec-dir": 4}

# stageimage reads with fgets into `char line[2048]`, so it stores at most 2047
# bytes and refuses a line that filled the buffer without a newline. The test
# below is that C test transcribed, which is why it runs on the untrimmed line:
# a CRLF line spends one of the 2046 accepted content bytes on the carriage
# return.
LINE_BUFFER = 2048
MAX_LINE = LINE_BUFFER - 2

#
# The allowlist. One table per manifest, named by the manifest's stem, because
# a manifest this table does not name is a hard error and a new manifest must
# therefore arrive through the guard. `setid` declares every set-id path with
# the mode, the owner and the host file it is staged from; the owner is per
# entry rather than an assertion that root owns them all, since the test image
# deliberately stages a setuid file owned by uid 5 and a setgid file owned by
# gid 5 to prove the bit grants the file's owner whoever that is, and the host
# source is declared because a path alone does not say what runs there:
# pointing /bin/su at build/vendor/busybox/busybox satisfies every other rule
# and hands a setuid-root busybox to the image. `trusted` names the files that
# must be staged, must NOT be set-id, and must be at exactly the mode, owner and
# host source named here, because "not set-id" is not the whole of what an image
# has to get right. Two kinds are in it. The first is a program root runs:
# `/bin/login` is exec'd by init as root, and `/bin/sh` is what login and
# dropbear exec for root, so either one staged 0777 lets any account replace the
# program root runs next, with no set-id bit anywhere in sight. The second is a
# file root READS to decide who somebody is: `/etc/shadow` writable by anyone is
# a root password anyone may choose, and so, one step removed, are `/etc/passwd`,
# `/etc/group`, `/etc/shells`, `/etc/profile` and root's `authorized_keys`.
# The image half checks the three fields an image records; the manifest half
# checks the host source as well, and `None` there is an inline `text` payload.
#
# The test account's own `authorized_keys` is deliberately NOT here: it is that
# account's file, under a home the account owns, and replacing it is not a climb. `sentinel` names one ordinary
# node the image inventory must report exactly; it is required of a manifest
# that declares no set-id entry, which would otherwise have nothing positive to
# find and would read a misparsed image as clean.
#
ALLOWLIST = {
    "os-real": {
        # Privilege climbs through the set-id busybox and nowhere else: one
        # binary built from busybox's own source with three applets in it,
        # staged three times under the names it dispatches on. The main busybox
        # stays unprivileged, since a setuid one would run every one of its
        # BB_SUID_REQUIRE and BB_SUID_MAYBE applets as root unless an
        # /etc/busybox.conf confined it, and that config path trips a busybox
        # use-after-free the kernel's strict munmap faults.
        "setid": {
            "/bin/su": (0o4755, 0, 0, "build/vendor/busybox/busybox-suid"),
            "/bin/passwd": (0o4755, 0, 0, "build/vendor/busybox/busybox-suid"),
            "/bin/login": (0o4755, 0, 0, "build/vendor/busybox/busybox-suid"),
        },
        "trusted": {
            "/bin/busybox": (0o755, 0, 0, "build/vendor/busybox/busybox"),
            "/bin/sh": (0o755, 0, 0, "build/vendor/busybox/busybox"),
            "/bin/dropbearmulti": (0o755, 0, 0,
                                   "build/vendor/dropbear/dropbearmulti"),
            "/etc/passwd": (0o644, 0, 0, None),
            "/etc/shadow": (0o600, 0, 0, "krffs/shadow"),
            "/etc/group": (0o644, 0, 0, None),
            "/etc/shells": (0o644, 0, 0, None),
            "/etc/profile": (0o644, 0, 0, None),
            "/root/.ssh/authorized_keys": (0o600, 0, 0,
                                           "krffs/authorized_keys"),
        },
        "sentinel": None,
        "init": "init-login",
    },
    "test": {
        # suidprobe's fixtures (the same helper at several modes and owners),
        # sudoprobe's reference sudo, suidmusl's setuid copy, which proves
        # libc's view of a set-id exec, and pwauthmusl's /su, which is the
        # shipped binary at the shipped mode rather than a copy of it.
        "setid": {
            "/suidhelper.suid": (0o4755, 0, 0, "build/tests/suidhelper"),
            "/suidhelper.u5": (0o4755, 5, 0, "build/tests/suidhelper"),
            "/suidhelper.sgid": (0o2755, 0, 5, "build/tests/suidhelper"),
            "/suidhelper.nox": (0o4644, 0, 0, "build/tests/suidhelper"),
            "/sudo": (0o4755, 0, 0, "build/utils/sbin/sudo"),
            "/su": (0o4755, 0, 0, "build/utils/sbin/su"),
            "/passwd": (0o4755, 0, 0, "build/utils/sbin/passwd"),
            "/musl/suidmusl.suid": (0o4755, 0, 0,
                                    "build/tests/musl/suidmusl"),
        },
        "trusted": {},
        "sentinel": None,
        "init": None,
    },
    "os-simple": {
        # The same trust boundary as os-real's, drawn around this project's own
        # code instead of busybox's: su and passwd read /etc/shadow, so they
        # carry the bit, and login and adduser are exec'd by something that is
        # root already.
        "setid": {
            "/bin/su": (0o4755, 0, 0, "build/utils/sbin/su"),
            "/bin/passwd": (0o4755, 0, 0, "build/utils/sbin/passwd"),
        },
        "trusted": {
            "/bin/login": (0o755, 0, 0, "build/utils/sbin/login"),
            "/bin/adduser": (0o755, 0, 0, "build/utils/sbin/adduser"),
            "/bin/ish": (0o755, 0, 0, "build/ish/ish"),
            "/bin/sh": (0o755, 0, 0, "build/ish/ish"),
            "/etc/passwd": (0o644, 0, 0, None),
            "/etc/shadow": (0o600, 0, 0, "krffs/shadow"),
            "/etc/group": (0o644, 0, 0, None),
            "/etc/shells": (0o644, 0, 0, None),
        },
        "sentinel": ("/hello.txt", 0o644, 0, 0),
        "init": "init-login",
    },
    "disk-test": {
        "setid": {},
        "trusted": {},
        "sentinel": ("/blockprobe", 0o755, 0, 0),
        "init": None,
    },
    "persist-test": {
        "setid": {},
        "trusted": {},
        "sentinel": ("/persistprobe", 0o755, 0, 0),
        "init": None,
    },
    "cred-persist-test": {
        "setid": {},
        "trusted": {},
        "sentinel": ("/creddisk", 0o755, 0, 0),
        "init": None,
    },
    "login-test-strict": {
        # The console-login fixtures: the same image but for the init each
        # declares, and neither stages /bin/login. The strict one must refuse
        # and the permissive one must reach a root shell, which is what makes
        # the strict one's silence mean anything.
        "setid": {},
        "trusted": {
            "/sbin/init": (0o755, 0, 0, "build/utils/sbin/init-login"),
        },
        "sentinel": ("/etc/motd", 0o644, 0, 0),
        "init": "init-login",
    },
    "login-test-shell": {
        "setid": {},
        "trusted": {
            "/sbin/init": (0o755, 0, 0, "build/utils/sbin/init-shell"),
        },
        "sentinel": ("/etc/motd", 0o644, 0, 0),
        "init": "init-shell",
    },
    "shutdown-test": {
        # The shutdown fixture stages the shipped setuid `su` so one phase can
        # ask a bare `halt` as somebody who is not root and be refused.
        "setid": {
            "/bin/su": (0o4755, 0, 0, "build/utils/sbin/su"),
        },
        "trusted": {
            "/bin/busybox": (0o755, 0, 0, "build/vendor/busybox/busybox"),
        },
        "sentinel": None,
        "init": "init-shell",
    },
    "stress-test": {
        "setid": {},
        "trusted": {},
        "sentinel": ("/stressprobe", 0o755, 0, 0),
        "init": None,
    },
}

#
# The v2 on-disk node, byte for byte. struct krffs_node (krffs/krffs_node.h) is
# `#pragma pack(1)`, so this one format string is the whole layout: magic,
# type, id, the 256-byte name buffer, mode, uid, gid, three 16-byte timestamps,
# size, previous_node_size, data_size, parent_id.
#
NODE_FORMAT = "<HBQ256sHIIqqqqqqQQQQ"
NODE_SIZE = struct.calcsize(NODE_FORMAT)
SIZE_FIELD = struct.calcsize("<HBQ256sHIIqqqqqq")
KRFFS_MAGIC = 0x4B52
NODE_ROOT = 0
NODE_FREE = 1
NODE_RESERVED = 2

# The header is 357 packed bytes. A format string that sizes differently is a
# transcription error, and the walk would read every field from the wrong
# place, so say so here rather than report a corrupt image.
if NODE_SIZE != 357:
    raise SystemExit("check-setid: the node layout reads as %d bytes, not the "
                     "357 of the v2 ABI" % NODE_SIZE)


def manifest_stem(path):
    """The allowlist key a manifest path names, `krffs/manifests/x.manifest`
    being `x`."""
    name = os.path.basename(path)

    return name[:-len(".manifest")] if name.endswith(".manifest") else name


def fmt_owner(uid, gid):
    """The manifest's owner spelling, `uid:gid`."""
    return "%u:%u" % (uid, gid)


def fmt_entry(mode, uid, gid):
    """The manifest's mode spelling, `0mmmm:uid:gid`."""
    return "0%o:%s" % (mode, fmt_owner(uid, gid))


def strtoul(s, base):
    """Convert a numeric prefix of `s` the way C's strtoul does.

    Returns the value, the unconverted tail, and whether any digit was
    converted. Leading whitespace and a sign are consumed, and a negative value
    wraps into 64 bits, which is what makes `-1` fail the range tests below
    rather than read as a small number. On no conversion the tail is the whole
    original string, so a caller comparing `end == start` sees the same thing C
    does.
    """
    i = 0
    while i < len(s) and s[i] in " \t\n\v\f\r":
        i += 1
    negative = False
    if i < len(s) and s[i] in "+-":
        negative = s[i] == "-"
        i += 1
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"[:base]
    start = i
    while i < len(s) and s[i].lower() in digits:
        i += 1
    if i == start:
        return 0, s, False
    value = int(s[start:i], base)
    if negative:
        value = (-value) & 0xFFFFFFFFFFFFFFFF

    return value, s[i:], True


def parse_mode(token):
    """The (mode, uid, gid) of a manifest mode field, or None if malformed.

    This is `parse_mode` (krffs/stageimage.krffs.c) with its exact verdicts,
    which is the point of it: the mode is octal and at most 07777, the tail
    after it is either empty or an owner introduced by ':', and an owner is two
    complete decimal fields. So `0999` and `08` are malformed rather than
    partly read, and `04755:0` is malformed rather than read as mode 04755
    owned by 0, which is how a guard that split on ':' misread it.
    """
    if token == "":
        return None
    value, rest, _ = strtoul(token, 8)
    if value > PERM or (rest != "" and not rest.startswith(":")):
        return None
    if rest == "":
        return value, 0, 0

    uid, rest, converted = strtoul(rest[1:], 10)
    if not converted or not rest.startswith(":"):
        return None
    gid, rest, converted = strtoul(rest[1:], 10)
    if not converted or rest != "":
        return None
    if uid > 0xFFFFFFFF or gid > 0xFFFFFFFF:
        return None

    return value, uid, gid


def tokenize(line):
    """Split a manifest line into fields the way strtok_r(" \\t") does."""
    return [field for field in re.split("[ \t]+", line) if field != ""]


def parse_manifest(path):
    """Parse a manifest into directives and the lines stageimage would refuse.

    A directive is (lineno, verb, mode, uid, gid, guest, source), the source
    being the host operand of `file`, `optional-file` and `exec-dir` and None
    for the two verbs that have none. Returns the directives and a list of
    diagnostics.

    The file is opened byte for byte: latin-1 so that a length is a byte count
    and a stray high byte cannot raise a decode error out of a build gate, and
    with the newline translation off so that only '\\n' ends a line. Python's
    universal newlines would end one on a bare carriage return where `fgets`
    does not, which is enough to hide a privileged path behind a declared one:
    `file 04755:0:0 /bin/su<CR>#x host` stages `/bin/su#x` and reads here as a
    declared `/bin/su` followed by a comment.
    """
    directives = []
    problems = []
    name = os.path.basename(path)

    with open(path, "r", encoding="latin-1", newline="\n") as f:
        for lineno, raw in enumerate(f, 1):
            stored = raw[:LINE_BUFFER - 1]
            if len(stored) == LINE_BUFFER - 1 and not stored.endswith("\n"):
                problems.append("%s:%d: line is longer than the %d bytes "
                                "stageimage accepts"
                                % (name, lineno, MAX_LINE))
                continue
            line = stored.rstrip("\r\n")
            fields = tokenize(line)
            if not fields or fields[0].startswith("#"):
                continue
            if "\0" in line:
                problems.append("%s:%d: NUL byte in a directive; stageimage "
                                "reads the line with strlen and would stage "
                                "only the part before it" % (name, lineno))
                continue
            if max(line) > "\x7f":
                problems.append("%s:%d: non-ASCII byte in a directive; a "
                                "guest path must be ASCII, since the image "
                                "inventory names nodes by their bytes"
                                % (name, lineno))
                continue
            verb = fields[0]
            if verb not in VERBS:
                problems.append("%s:%d: unknown directive '%s'; the "
                                "grammar is %s"
                                % (name, lineno, verb, ", ".join(VERBS)))
                continue
            if len(fields) < 3:
                problems.append("%s:%d: malformed '%s' directive; it needs a "
                                "mode and a path" % (name, lineno, verb))
                continue
            parsed = parse_mode(fields[1])
            if parsed is None:
                problems.append("%s:%d: malformed mode '%s' in a '%s' "
                                "directive" % (name, lineno, fields[1], verb))
                continue
            if len(fields) < ARITY[verb]:
                problems.append("%s:%d: '%s' needs a host %s"
                                % (name, lineno, verb,
                                   "dir" if verb == "exec-dir" else "path"))
                continue
            mode, uid, gid = parsed
            source = fields[3] if verb in HOST_VERBS else None
            directives.append((lineno, verb, mode, uid, gid, fields[2],
                               source))

    return directives, problems


# The one mode and owner an init may be staged at. See check_init_line.
INIT_MODE = 0o755


def check_init_line(name, directives, want):
    """Check which init a manifest stages. Returns a list of problems.

    Neither init artifact is named plain `init`, so a manifest left on the old
    path fails to stage rather than silently getting whichever one the tree
    happens to build; and which one an image gets is a fact worth asserting
    rather than reading, since the two differ in whether the console can fall
    back to an unauthenticated root shell. A manifest whose entry declares no
    init must stage none: the test images boot no init at all, and one that
    grew an init line would be running a different system than the one its
    probes were written against.
    """
    problems = []
    staged = [(lineno, verb, mode, uid, gid, source)
              for lineno, verb, mode, uid, gid, guest, source
              in directives if guest == "/sbin/init" and verb in FILE_VERBS]

    if want is None:
        for lineno, _verb, _m, _u, _g, _source in staged:
            problems.append("%s:%d: stages /sbin/init and the allowlist "
                            "declares none for this image" % (name, lineno))
        return problems
    if not staged:
        return ["%s: stages no /sbin/init and the allowlist declares %s"
                % (name, want)]
    wanted_source = "build/utils/sbin/" + want
    for lineno, verb, mode, uid, gid, source in staged:
        # The whole path and not its last component: a basename test would take
        # any */init-login, and the artifact `check_inits` scans is the one at
        # this path, so the two halves must be talking about the same file.
        if source != wanted_source:
            problems.append("%s:%d: stages /sbin/init from %s and the "
                            "allowlist declares %s"
                            % (name, lineno, source if source else "an inline payload",
                               wanted_source))
        # And mandatory: an `optional-file` that is not there stages nothing,
        # and an image with no init at all is not the image this declares.
        if verb != "file":
            problems.append("%s:%d: stages /sbin/init with '%s'; an image that "
                            "declares an init must carry one, so the directive "
                            "must be 'file'" % (name, lineno, verb))
        # And at the one mode and owner an init may have. The kernel execs this
        # file as root before anything else runs, so it is the same trust
        # boundary a set-id program is, and a persistent image whose init any
        # account may rewrite hands that account root at the next boot. The mode
        # is a rule here rather than a per-image datum: no image has a reason to
        # stage its init any other way.
        if (mode, uid, gid) != (INIT_MODE, 0, 0):
            problems.append("%s:%d: stages /sbin/init %s; the kernel execs it "
                            "as root, so it must be %s"
                            % (name, lineno, fmt_entry(mode, uid, gid),
                               fmt_entry(INIT_MODE, 0, 0)))

    return problems


# The directories on the way to a program root runs, which are part of the same
# trust boundary the program is. This kernel decides `unlink` and `rename` on
# the parent directory's write permission, so a `/bin` staged 0777 lets any
# account replace a correctly declared `/bin/login` with a program of their own
# and wait for init to exec it. A declared mode on the file alone does not make
# the file the one that runs.
#
# The rule is a rule and not a table: every ancestor of a declared path must be
# staged, owned by root, and writable by nobody else. `/` is not checked here
# because no manifest stages it; `stageimage` forces the root node to 0:0 and
# the image half reads it back.
def ancestors_of(guest):
    """Yield the directory paths above `guest`, nearest last."""
    parts = [p for p in guest.split("/") if p]
    out = []
    for i in range(1, len(parts)):
        out.append("/" + "/".join(parts[:i]))

    return out


def declared_paths(allow):
    """Every path an allowlist entry says root runs or trusts."""
    paths = sorted(allow["setid"]) + sorted(allow["trusted"])
    if allow.get("init"):
        paths.append("/sbin/init")

    return paths


def check_ancestors(where, allow, look):
    """Check the directories above every declared path.

    `look` answers a directory path with (mode, uid, gid) or None. Returns a
    list of problems.
    """
    problems = []
    seen = []
    for guest in declared_paths(allow):
        for parent in ancestors_of(guest):
            if parent in seen:
                continue
            seen.append(parent)
            found = look(parent)
            if found is None:
                problems.append("%s: %s holds a program root runs and no 'dir' "
                                "line stages it; a directory that comes into "
                                "being on the way to a file takes a mode "
                                "nobody wrote down" % (where, parent))
                continue
            mode, uid, gid = found
            if (uid, gid) != (0, 0) or (mode & 0o022) != 0:
                problems.append("%s: %s is staged %s and holds a program root "
                                "runs; a directory anyone else may write is a "
                                "way to replace what is in it"
                                % (where, parent, fmt_entry(mode, uid, gid)))

    return problems


def check_manifest(path, table=ALLOWLIST):
    """Check one manifest against the allowlist. Returns a list of problems."""
    name = os.path.basename(path)
    stem = manifest_stem(path)
    if stem not in table:
        return ["%s: no allowlist names this manifest; add its entry to "
                "ALLOWLIST in tools/check_setid.py (a new manifest declares "
                "its set-id set, or declares that it has none)" % name]

    allow = table[stem]
    declared = allow["setid"]
    directives, problems = parse_manifest(path)
    # What the image ends up holding for a path. `stage_file` refuses a path it
    # has already staged, so the first directive for a path is the only one,
    # and a second is reported here rather than left to fail the build.
    staged = {}

    problems.extend(check_init_line(name, directives, allow.get("init")))
    for lineno, verb, mode, uid, gid, guest, source in directives:
        if verb in FILE_VERBS:
            if guest in staged:
                problems.append("%s:%d: %s is staged more than once; "
                                "stageimage refuses a repeated path"
                                % (name, lineno, guest))
            else:
                staged[guest] = (lineno, mode, uid, gid, source)
        if not (mode & SETID):
            continue
        if verb == "exec-dir":
            problems.append("%s:%d: 'exec-dir' stages mode %s into %s; a "
                            "set-id exec-dir privileges every executable that "
                            "happens to be in the host directory, so it is "
                            "refused. Name the one binary with 'file'."
                            % (name, lineno, fmt_entry(mode, uid, gid), guest))
            continue
        if verb == "dir":
            problems.append("%s:%d: 'dir' stages %s at mode %s; KRFFS has no "
                            "setgid-directory inheritance, so nothing would "
                            "ever read the bit. Sticky (01000) is legal."
                            % (name, lineno, guest,
                               fmt_entry(mode, uid, gid)))
            continue
        if guest not in declared:
            problems.append("%s:%d: '%s' stages %s set-id (%s) and the %s "
                            "allowlist does not declare it"
                            % (name, lineno, verb, guest,
                               fmt_entry(mode, uid, gid), stem))
            continue
        want_mode, want_uid, want_gid, want_source = declared[guest]
        if mode != want_mode or uid != want_uid or gid != want_gid:
            problems.append("%s:%d: %s is staged %s but the %s allowlist "
                            "declares %s"
                            % (name, lineno, guest, fmt_entry(mode, uid, gid),
                               stem, fmt_entry(want_mode, want_uid, want_gid)))
        elif source is None:
            problems.append("%s:%d: '%s' stages the declared set-id path %s "
                            "inline; the %s allowlist declares it staged from "
                            "%s, and a privileged file is a program copied "
                            "from the host"
                            % (name, lineno, verb, guest, stem, want_source))
        elif source != want_source:
            problems.append("%s:%d: %s is staged from %s but the %s allowlist "
                            "declares it staged from %s"
                            % (name, lineno, guest, source, stem, want_source))

    for guest in sorted(declared):
        want_mode, want_uid, want_gid, want_source = declared[guest]
        want = fmt_entry(want_mode, want_uid, want_gid)
        if guest in staged:
            lineno, mode, uid, gid, source = staged[guest]
            if (mode, uid, gid) == (want_mode, want_uid, want_gid):
                continue
            problems.append("%s:%d: %s is declared %s but is staged %s; the "
                            "declared privilege is gone"
                            % (name, lineno, guest, want,
                               fmt_entry(mode, uid, gid)))
        else:
            problems.append("%s: %s is declared %s but no directive stages it"
                            % (name, guest, want))

    # The LAST `dir` for a path is the one that decides its mode and owner, the
    # way `stage_dir` does: a repeated directive re-applies both to the node it
    # finds. `exec-dir` is not a directory declaration at all; its mode belongs
    # to the files it stages, and the guest directory it stages them into is
    # either declared by a `dir` of its own or created implicitly at 0755 by
    # `resolve_dir`, which is a mode nobody wrote down.
    dirs = {}
    for lineno, verb, mode, uid, gid, guest, _source in directives:
        if verb == "dir":
            dirs[guest] = (mode & PERM, uid, gid)
    problems.extend(check_ancestors(name, allow, dirs.get))

    for guest in sorted(allow["trusted"]):
        want_mode, want_uid, want_gid, want_source = allow["trusted"][guest]
        want = fmt_entry(want_mode, want_uid, want_gid)
        if guest not in staged:
            problems.append("%s: %s is not staged; the allowlist requires it "
                            "to be present, unprivileged and exactly as "
                            "declared" % (name, guest))
            continue
        lineno, mode, uid, gid, source = staged[guest]
        if mode & SETID:
            problems.append("%s:%d: %s is staged set-id (%s); it must stay "
                            "unprivileged, and privilege must climb through "
                            "the declared paths alone"
                            % (name, lineno, guest, fmt_entry(mode, uid, gid)))
        elif (mode, uid, gid) != (want_mode, want_uid, want_gid):
            problems.append("%s:%d: %s is declared %s but is staged %s; a "
                            "program root runs, and a file root reads to "
                            "decide who somebody is, are trust boundaries too"
                            % (name, lineno, guest, want,
                               fmt_entry(mode, uid, gid)))
        elif source != want_source:
            problems.append("%s:%d: %s is declared staged from %s but this "
                            "stages %s" % (name, lineno, guest, want_source,
                                           source))

    return problems


class ImageError(Exception):
    """A staged image that cannot be walked or read as a KRFFS."""


def unpack_node(region, offset):
    """Decode one on-disk node from `region` at `offset`.

    The name is decoded strictly. A node name is a guest path component, the
    manifest sweep refuses a non-ASCII byte in a directive, and a lossy decode
    would fold two distinct names onto one path, which is how a set-id node
    hides behind a sibling.
    """
    fields = struct.unpack_from(NODE_FORMAT, region, offset)
    raw = fields[3].split(b"\0", 1)[0]
    try:
        name = raw.decode("ascii")
    except UnicodeDecodeError:
        raise ImageError("the node at offset %d has a name that is not ASCII "
                         "(%r), so no path names it unambiguously"
                         % (offset, raw))

    return {
        "magic": fields[0],
        "type": fields[1],
        "id": fields[2],
        "name": name,
        "mode": fields[4],
        "uid": fields[5],
        "gid": fields[6],
        "size": fields[13],
        "previous_node_size": fields[14],
        "data_size": fields[15],
        "parent_id": fields[16],
        "offset": offset,
    }


def walk_nodes(region):
    """Walk the node chain from the root and return every node in ring order.

    This is fsck.krffs's walk: each node advances by its own `size`, wrapping
    once at the region end, with the iteration capped at the most nodes the
    region could hold, so a node whose size does not advance the walk is a
    corrupt chain rather than an endless one. Every step is checked for range,
    signature and a known type, exactly as there.

    It then requires the ring to close on the size the root records in
    `previous_node_size`, which fsck does not, because the walk alone cannot
    tell a whole file system from a prefix of one. A region whose size happens
    to equal a node's offset tiles perfectly and stops at the root, reporting
    every node before the cut and none after it; the closure test is the only
    thing that ties the region handed in to the file system that is there.
    """
    size = len(region)
    if size < NODE_SIZE * 2:
        raise ImageError("region of %d bytes is too small to hold a file "
                         "system" % size)

    nodes = []
    position = 0
    previous = 0
    steps = 0
    max_steps = size // NODE_SIZE + 1

    while True:
        steps += 1
        if steps > max_steps:
            raise ImageError("the node chain does not return to the root "
                             "within the region bound; the image is corrupt")
        if previous > position:
            raise ImageError("the node chain links backwards to offset %d; "
                             "the image is corrupt" % position)
        if position + NODE_SIZE > size:
            raise ImageError("node at offset %d lies outside the %d-byte "
                             "region" % (position, size))
        node = unpack_node(region, position)
        if node["magic"] != KRFFS_MAGIC:
            raise ImageError("no file system signature in the node at offset "
                             "%d (found 0x%04x)" % (position, node["magic"]))
        if steps == 1 and node["type"] != NODE_ROOT:
            raise ImageError("the node at the region start is not the root "
                             "node (type %d)" % node["type"])
        if node["type"] not in (NODE_ROOT, NODE_FREE, NODE_RESERVED):
            raise ImageError("node at offset %d has unknown type %d"
                             % (position, node["type"]))
        if node["size"] < NODE_SIZE or node["size"] > size:
            raise ImageError("node at offset %d claims a size of %d bytes, "
                             "which no node in a %d-byte region can have"
                             % (position, node["size"], size))
        nodes.append(node)

        previous = position
        position += node["size"]
        if position >= size:
            position -= size
        if position == 0:
            break

    if nodes[0]["previous_node_size"] != nodes[-1]["size"]:
        raise ImageError("the node ring does not close: the root records a "
                         "last node of %d bytes and the walk ended on one of "
                         "%d, so the region handed in is not the whole file "
                         "system" % (nodes[0]["previous_node_size"],
                                     nodes[-1]["size"]))

    return nodes


def node_paths(nodes):
    """Rebuild the absolute path of every reserved node from `parent_id`.

    The root's id is 0 as a parent, and `krffs_mix_id` never returns 0, so a
    chain terminates only at the root; anything else is an unresolvable or
    cyclic parent and is an error rather than a skipped node, since a node the
    inventory cannot name is a node it cannot clear. Two nodes resolving to one
    path are an error for the same reason: the second would overwrite the first
    and the inventory would never look at it.
    """
    reserved = [n for n in nodes if n["type"] == NODE_RESERVED]
    by_id = {}
    for node in reserved:
        if node["id"] in by_id:
            raise ImageError("two reserved nodes share id %u ('%s' at offset "
                             "%d and '%s' at offset %d), so no path is "
                             "unambiguous"
                             % (node["id"], by_id[node["id"]]["name"],
                                by_id[node["id"]]["offset"], node["name"],
                                node["offset"]))
        by_id[node["id"]] = node

    paths = {}
    for node in reserved:
        parts = []
        walker = node
        seen = set()
        while True:
            parts.append(walker["name"])
            parent = walker["parent_id"]
            if parent == 0:
                break
            if parent in seen:
                raise ImageError("the parent chain of node '%s' at offset %d "
                                 "is cyclic" % (node["name"], node["offset"]))
            seen.add(parent)
            walker = by_id.get(parent)
            if walker is None:
                raise ImageError("node '%s' at offset %d names parent id %u, "
                                 "which no node carries"
                                 % (node["name"], node["offset"], parent))
        path = "/" + "/".join(reversed(parts))
        if path in paths:
            raise ImageError("two reserved nodes resolve to the path %s (at "
                             "offsets %d and %d), so one of them would go "
                             "uninspected"
                             % (path, paths[path]["offset"], node["offset"]))
        paths[path] = node

    return paths


def read_region(image, offset, size):
    """Read the `size` bytes at `offset`, checking the region bounds."""
    total = os.path.getsize(image)
    if offset < 0 or offset > total:
        raise ImageError("offset %d is past the end of the %d-byte image"
                         % (offset, total))
    if size is None:
        size = total - offset
    if size <= 0 or offset + size > total:
        raise ImageError("region (offset %d, size %d) does not fit in the "
                         "%d-byte image" % (offset, size, total))
    with open(image, "rb") as f:
        f.seek(offset)
        region = f.read(size)
    if len(region) != size:
        raise ImageError("short read of the region at offset %d" % offset)

    return region


def check_image(image, manifest, offset=0, size=None, table=ALLOWLIST):
    """Inventory a staged image against the allowlist `manifest` owns.

    Returns (problems, summary). The root node is deliberately not inventoried:
    KRFFS_MODE_DIRTY is 0004000, the setuid bit's own place, and the root is
    the one node that carries it, so a dirty image would otherwise report its
    root as setuid root.

    The allowlist entry is checked here and not only in the sweep, because this
    half runs from mkimage.sh and mkdisk.sh and the sweep runs from the source
    gate: an entry with neither a declared set-id path nor a sentinel would
    leave this half with nothing positive to find, and it would pass over any
    region at all.
    """
    name = os.path.basename(manifest)
    stem = manifest_stem(manifest)
    if stem not in table:
        return (["%s: no allowlist names this manifest; the inventory cannot "
                 "say what %s may carry" % (name, os.path.basename(image))],
                "")

    allow = table[stem]
    declared = allow["setid"]
    sentinel = allow["sentinel"]
    if not declared and sentinel is None:
        return (["allowlist: %s declares no set-id entry and no sentinel; the "
                 "image inventory would have nothing to find" % stem], "")

    try:
        region = read_region(image, offset, size)
        nodes = walk_nodes(region)
        paths = node_paths(nodes)
    except (ImageError, OSError) as e:
        return ["%s: %s" % (os.path.basename(image), e)], ""

    problems = []
    found = {}
    for path in sorted(paths):
        node = paths[path]
        if node["mode"] & SETID:
            found[path] = node

    for path in sorted(found):
        node = found[path]
        mode = node["mode"] & PERM
        entry = fmt_entry(mode, node["uid"], node["gid"])
        if path not in declared:
            problems.append("%s: %s is staged set-id (%s) and the %s "
                            "allowlist does not declare it"
                            % (os.path.basename(image), path, entry, stem))
            continue
        want_mode, want_uid, want_gid, _ = declared[path]
        if mode != want_mode or node["uid"] != want_uid \
                or node["gid"] != want_gid:
            problems.append("%s: %s is staged %s but the %s allowlist "
                            "declares %s"
                            % (os.path.basename(image), path, entry, stem,
                               fmt_entry(want_mode, want_uid, want_gid)))

    for path in sorted(declared):
        if path not in found:
            want_mode, want_uid, want_gid, _ = declared[path]
            problems.append("%s: %s is declared %s but the image carries no "
                            "such set-id node"
                            % (os.path.basename(image), path,
                               fmt_entry(want_mode, want_uid, want_gid)))

    def image_dir(guest):
        node = paths.get(guest)
        if node is None or not (node["mode"] & 0o40000):
            return None

        return (node["mode"] & PERM, node["uid"], node["gid"])

    problems.extend(check_ancestors(os.path.basename(image), allow, image_dir))

    for path in sorted(allow["trusted"]):
        want_mode, want_uid, want_gid, _ = allow["trusted"][path]
        node = paths.get(path)
        if node is None:
            problems.append("%s: %s is not in the image; the allowlist "
                            "requires it to be present and unprivileged"
                            % (os.path.basename(image), path))
        elif node["mode"] & SETID:
            problems.append("%s: %s is staged set-id (%s); it must stay "
                            "trusted"
                            % (os.path.basename(image), path,
                               fmt_entry(node["mode"] & PERM, node["uid"],
                                         node["gid"])))
        elif (node["mode"] & PERM, node["uid"], node["gid"]) != (
                want_mode, want_uid, want_gid):
            problems.append("%s: %s is declared %s but the image carries %s; "
                            "a program root runs, and a file root reads to "
                            "decide who somebody is, are trust boundaries too"
                            % (os.path.basename(image), path,
                               fmt_entry(want_mode, want_uid, want_gid),
                               fmt_entry(node["mode"] & PERM, node["uid"],
                                         node["gid"])))

    if sentinel is not None:
        path, want_mode, want_uid, want_gid = sentinel
        node = paths.get(path)
        if node is None:
            problems.append("%s: the sentinel %s is not in the image; the "
                            "walk found %d node(s) and nothing to confirm it "
                            "read them" % (os.path.basename(image), path,
                                           len(paths)))
        elif (node["mode"] & PERM) != want_mode or node["uid"] != want_uid \
                or node["gid"] != want_gid:
            problems.append("%s: the sentinel %s is %s, not the declared %s"
                            % (os.path.basename(image), path,
                               fmt_entry(node["mode"] & PERM, node["uid"],
                                         node["gid"]),
                               fmt_entry(want_mode, want_uid, want_gid)))

    summary = ("%s: %d node(s), %d reserved node(s), %d declared set-id"
               % (os.path.basename(image), len(nodes), len(paths),
                  len(declared)))

    return problems, summary


def validate_table(table=ALLOWLIST):
    """Check the allowlist itself: every manifest has something to find.

    A manifest that declares no set-id entry has no declared set for the image
    inventory to confirm, so it must name a sentinel; without one, an empty or
    misparsed walk would report the image clean.
    """
    problems = []
    for stem in sorted(table):
        allow = table[stem]
        if not allow["setid"] and allow["sentinel"] is None:
            problems.append("allowlist: %s declares no set-id entry and no "
                            "sentinel; the image inventory would have nothing "
                            "to find" % stem)
        for path, entry in sorted(allow["setid"].items()):
            if not (entry[0] & SETID):
                problems.append("allowlist: %s declares %s at mode 0%o, which "
                                "carries no set-id bit"
                                % (stem, path, entry[0]))

    return problems


def sweep(paths, table=ALLOWLIST, require_all=False):
    """Sweep the named manifests. Returns (problems, count)."""
    problems = validate_table(table)
    if require_all:
        for stem in sorted(table):
            expected = os.path.join(MANIFEST_DIR, stem + ".manifest")
            if expected not in paths:
                problems.append("allowlist: %s names %s, which is not in %s"
                                % (stem, stem + ".manifest", MANIFEST_DIR))
    for path in paths:
        try:
            problems.extend(check_manifest(path, table))
        except OSError as e:
            problems.append("%s: %s" % (os.path.basename(path), e))

    return problems, len(paths)


# The self-test's fixtures. Each sweep case is a manifest stem, the manifest
# text, and whether the sweep must accept it; a rejected case also names a
# fragment one of its findings must carry, so a case cannot pass for the wrong
# reason. Between them the cases cover every directive form and every failure
# mode the sweep is meant to have, including the reader's own boundaries: the
# byte where fgets gives up, a carriage return that ends a line for Python and
# not for C, a tab as a field separator, and a NUL that ends the line for C and
# not for Python.
OS_REAL_MINIMUM = ("dir 0755 /bin\n"
                   "dir 0755 /sbin\n"
                   "dir 0755 /etc\n"
                   "dir 0755 /root\n"
                   "dir 0700 /root/.ssh\n"
                   "file 0755 /bin/sh build/vendor/busybox/busybox\n"
                   "file 0755 /bin/dropbearmulti build/vendor/dropbear/dropbearmulti\n"
                   "text 0644 /etc/passwd x\n"
                   "file 0600 /etc/shadow krffs/shadow\n"
                   "text 0644 /etc/group x\n"
                   "text 0644 /etc/shells x\n"
                   "text 0644 /etc/profile x\n"
                   "file 0600 /root/.ssh/authorized_keys krffs/authorized_keys\n"
                   "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
                   "file 04755:0:0 /bin/su build/vendor/busybox/busybox-suid\n"
                   "file 04755:0:0 /bin/passwd build/vendor/busybox/busybox-suid\n"
                   "file 04755:0:0 /bin/login build/vendor/busybox/busybox-suid\n")

TEST_MINIMUM = ("dir 0755 /musl\n"
                "file 04755:0:0 /suidhelper.suid build/tests/suidhelper\n"
                "file 04755:5:0 /suidhelper.u5 build/tests/suidhelper\n"
                "file 02755:0:5 /suidhelper.sgid build/tests/suidhelper\n"
                "file 04644:0:0 /suidhelper.nox build/tests/suidhelper\n"
                "file 04755:0:0 /sudo build/utils/sbin/sudo\n"
                "file 04755:0:0 /su build/utils/sbin/su\n"
                "file 04755:0:0 /passwd build/utils/sbin/passwd\n"
                "file 04755:0:0 /musl/suidmusl.suid "
                "build/tests/musl/suidmusl\n")

OS_SIMPLE_MINIMUM = ("dir 0755 /bin\n"
                     "dir 0755 /sbin\n"
                     "dir 0755 /etc\n"
                     "text 0644 /etc/passwd x\n"
                     "file 0600 /etc/shadow krffs/shadow\n"
                     "text 0644 /etc/group x\n"
                     "text 0644 /etc/shells x\n"
                     "file 04755:0:0 /bin/su build/utils/sbin/su\n"
                     "file 04755:0:0 /bin/passwd build/utils/sbin/passwd\n"
                     "file 0755 /bin/login build/utils/sbin/login\n"
                     "file 0755 /bin/adduser build/utils/sbin/adduser\n"
                     "file 0755 /bin/ish build/ish/ish\n"
                     "file 0755 /bin/sh build/ish/ish\n")

# A line of exactly MAX_LINE content bytes, the longest stageimage accepts.
LONGEST_LINE = ("file 0755 /pad build/utils/cat"
                + " " * (MAX_LINE - len("file 0755 /pad build/utils/cat")))

SELFTEST_CASES = [
    ("a declared set-id file", "os-real", OS_REAL_MINIMUM, True, ""),
    ("a declared set-id optional-file", "os-real",
     OS_REAL_MINIMUM
     .replace("file 04755:0:0 /bin/su", "optional-file 04755:0:0 /bin/su")
     .replace("file 04755:0:0 /bin/passwd", "optional-file 04755:0:0 /bin/passwd")
     .replace("file 04755:0:0 /bin/login", "optional-file 04755:0:0 /bin/login"),
     True, ""),
    ("a declared set-id path staged as text", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "text 04755:0:0 /bin/su placeholder\n", False, "inline"),
    ("a declared set-id path from the wrong host file", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:0:0 /bin/su build/vendor/busybox/busybox\n",
     False, "declares it staged from"),
    ("an undeclared set-id optional-file", "os-real",
     OS_REAL_MINIMUM + "optional-file 04755:0:0 /bin/doom build/doom\n",
     False, "does not declare it"),
    ("an undeclared set-id text", "os-real",
     OS_REAL_MINIMUM + "text 02755:0:0 /etc/motd hello\\n",
     False, "does not declare it"),
    ("the test image's seven declared entries", "test", TEST_MINIMUM, True, ""),
    ("a set-id exec-dir", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hello, world\\n\n"
     "exec-dir 04755 / build/utils\n", False, "refused"),
    ("a plain exec-dir", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hello, world\\n\n"
     "exec-dir 0755 / build/utils\n", True, ""),
    ("a setgid dir", "os-real",
     OS_REAL_MINIMUM + "dir 02755 /home\n", False, "no setgid-directory"),
    ("a set-id dir on the root", "os-real",
     OS_REAL_MINIMUM + "dir 06755 /\n", False, "no setgid-directory"),
    ("a sticky dir", "os-real",
     OS_REAL_MINIMUM + "dir 1777 /tmp\n", True, ""),
    ("an undeclared set-id path", "os-real",
     OS_REAL_MINIMUM + "file 04755:0:0 /bin/evil build/utils/sbin/evil\n",
     False, "/bin/evil"),
    ("a declared path no manifest stages", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n",
     False, "no directive stages it"),
    ("a declared path at the wrong mode", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 02755:0:0 /bin/su build/utils/sbin/su\n",
     False, "allowlist declares 04755:0:0"),
    ("a declared path at the wrong owner", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:5:0 /bin/su build/utils/sbin/su\n",
     False, "allowlist declares 04755:0:0"),
    ("the malformed mode 0999", "os-real",
     OS_REAL_MINIMUM + "file 0999 /bin/x build/x\n", False, "malformed mode"),
    ("the malformed mode 08", "os-real",
     OS_REAL_MINIMUM + "file 08 /bin/x build/x\n", False, "malformed mode"),
    ("the malformed mode 04755:0", "os-real",
     OS_REAL_MINIMUM + "file 04755:0 /bin/x build/x\n",
     False, "malformed mode"),
    ("an unknown verb", "os-real",
     OS_REAL_MINIMUM + "symlink 0777 /bin/sh /bin/busybox\n",
     False, "unknown directive"),
    ("a directive short of its path", "os-real",
     OS_REAL_MINIMUM + "dir 0755\n", False, "needs a mode and a path"),
    ("a 'file' with no host path", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:0:0 /bin/su\n", False, "'file' needs a host path"),
    ("an 'optional-file' with no host path", "os-real",
     OS_REAL_MINIMUM + "optional-file 0755 /bin/doom\n",
     False, "'optional-file' needs a host path"),
    ("an 'exec-dir' with no host dir", "os-real",
     OS_REAL_MINIMUM + "exec-dir 0755 /bin\n",
     False, "'exec-dir' needs a host dir"),
    ("a 'text' with no payload", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt\n", True, ""),
    ("a full-line comment", "os-real",
     "# file 04755:0:0 /bin/evil build/evil\n"
     "   # an indented comment line\n" + OS_REAL_MINIMUM, True, ""),
    ("a trailing hash is not a comment", "os-real",
     OS_REAL_MINIMUM + "file 04755:0:0 /bin/evil build/evil # note\n",
     False, "/bin/evil"),
    ("tab-separated fields", "os-real",
     OS_REAL_MINIMUM.replace(" ", "\t"), True, ""),
    ("a carriage return inside a path", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:0:0 /bin/su\r#x build/utils/sbin/su\n",
     False, "does not declare it"),
    ("a NUL byte inside a path", "os-real",
     OS_REAL_MINIMUM + "file 0755 /bin/x\0y build/x\n", False, "NUL byte"),
    ("a non-ASCII byte in a directive", "os-real",
     OS_REAL_MINIMUM + "file 0755 /bin/caf\xe9 build/x\n",
     False, "non-ASCII byte"),
    ("a non-ASCII byte in a comment", "os-real",
     "# caf\xe9\n" + OS_REAL_MINIMUM, True, ""),
    ("an unprivileged program root runs, staged world-writable", "os-simple",
     OS_SIMPLE_MINIMUM.replace("file 0755 /bin/login", "file 0777 /bin/login")
     + "text 0644 /hello.txt hi\n", False, "trust boundaries too"),
    ("an unprivileged program staged from somewhere else", "os-simple",
     OS_SIMPLE_MINIMUM.replace("build/utils/sbin/login", "build/elsewhere/login")
     + "text 0644 /hello.txt hi\n", False, "declared staged from"),
    ("the shadow file staged readable by anyone", "os-real",
     OS_REAL_MINIMUM.replace("file 0600 /etc/shadow", "file 0644 /etc/shadow"),
     False, "trust boundaries too"),
    ("root's authorized_keys staged from somewhere else", "os-real",
     OS_REAL_MINIMUM.replace("krffs/authorized_keys", "krffs/test_authorized_keys"),
     False, "declared staged from"),
    ("the shell root's login execs staged writable", "os-real",
     OS_REAL_MINIMUM.replace("file 0755 /bin/sh", "file 0777 /bin/sh"),
     False, "trust boundaries too"),
    ("a safe directory declared again, unsafely", "os-simple",
     OS_SIMPLE_MINIMUM + "dir 0777 /bin\ntext 0644 /hello.txt hi\n",
     False, "may write is a way to replace"),
    ("an unsafe directory declared again, safely", "os-simple",
     "dir 0777 /bin\n" + OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hi\n",
     True, ""),
    ("an exec-dir standing in for the directory declaration", "os-simple",
     OS_SIMPLE_MINIMUM.replace("dir 0755 /bin\n", "exec-dir 0755 /bin build/utils\n")
     + "text 0644 /hello.txt hi\n", False, "no 'dir' line stages it"),
    ("the longest line stageimage accepts", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hi\n" + LONGEST_LINE + "\n",
     True, ""),
    ("a line one byte too long", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hi\n" + LONGEST_LINE + "x\n",
     False, "longer than"),
    ("the longest line ended with CRLF", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0644 /hello.txt hi\n" + LONGEST_LINE + "\r\n",
     False, "longer than"),
    ("an unknown manifest name", "login-test",
     "text 0644 /hello.txt hello\\n", False, "no allowlist names"),
    ("busybox staged setuid", "os-real",
     "file 04755:0:0 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:0:0 /bin/su build/utils/sbin/su\n",
     False, "must stay unprivileged"),
    ("busybox not staged at all", "os-real",
     "file 04755:0:0 /bin/su build/utils/sbin/su\n",
     False, "is not staged"),
    ("a path staged twice", "os-real",
     OS_REAL_MINIMUM + "file 0755 /bin/su build/utils/sbin/su\n",
     False, "staged more than once"),
    ("su without its setuid bit", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 0755 /bin/su build/utils/sbin/su\n",
     False, "declared privilege is gone"),
    ("su owned 5:0", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "file 04755:5:0 /bin/su build/utils/sbin/su\n",
     False, "allowlist declares 04755:0:0"),
]

# The allowlist's own cases. The table is what makes a future manifest declare
# itself, so the rules that police it are exercised rather than assumed.
TABLE_CASES = [
    ("a table entry with nothing to find",
     {"x": {"setid": {}, "trusted": {}, "sentinel": None}},
     "nothing to find"),
    ("a declared entry carrying no set-id bit",
     {"x": {"setid": {"/x": (0o755, 0, 0, "build/x")}, "trusted": {},
            "sentinel": None}},
     "carries no set-id bit"),
]

#
# The image inventory's fixtures. The half that reads the artifact has no bad
# image anywhere in the build to fail on, so the regions it must reject are
# built here out of the same NODE_FORMAT it decodes: a root, four reserved
# nodes and a trailing free node that closes the ring. Each node is one header
# wide except the free one, which takes the remainder, so truncating the region
# to a multiple of the header size tiles it perfectly and is exactly the cut
# that reads clean without the ring-closure test.
#
IMAGE_TABLE = {
    "img-fixture": {
        "setid": {"/bin/su": (0o4755, 0, 0, "build/utils/sbin/su")},
        "trusted": {
            "/bin/busybox": (0o755, 0, 0, "build/vendor/busybox/busybox"),
        },
        "sentinel": ("/hello.txt", 0o644, 0, 0),
    },
    "img-blind": {"setid": {}, "trusted": {}, "sentinel": None},
}

IMAGE_TOTAL = 4096
IMAGE_NODES = [
    (NODE_ROOT, 5381, b"", 0o755, 0, 0, 0),
    (NODE_RESERVED, 11, b"bin", 0o40755, 0, 0, 0),
    (NODE_RESERVED, 12, b"su", 0o4755, 0, 0, 11),
    (NODE_RESERVED, 13, b"busybox", 0o755, 0, 0, 11),
    (NODE_RESERVED, 14, b"hello.txt", 0o644, 0, 0, 0),
]


def build_region(nodes, total=IMAGE_TOTAL):
    """Pack `nodes` into a region, closed by a free node of the remainder."""
    sizes = [NODE_SIZE] * len(nodes)
    sizes.append(total - NODE_SIZE * len(nodes))
    packed = list(nodes) + [(NODE_FREE, 0, b"", 0, 0, 0, 0)]
    region = b""
    previous = sizes[-1]
    for (kind, node_id, name, mode, uid, gid, parent), size in zip(packed,
                                                                  sizes):
        region += struct.pack(NODE_FORMAT, KRFFS_MAGIC, kind, node_id, name,
                              mode, uid, gid, 0, 0, 0, 0, 0, 0,
                              size, previous, 0, parent)
        region += b"\0" * (size - NODE_SIZE)
        previous = size

    return region


def fixture(index=None, node=None):
    """The fixture region, with one node optionally replaced."""
    nodes = list(IMAGE_NODES)
    if index is not None:
        nodes[index] = node

    return build_region(nodes)


def oversized_fixture(index):
    """The fixture region with node `index` claiming more than the region.

    A node whose size runs past the end advances the walk by an arbitrary
    amount once the wrap subtracts the region size, so it can skip over nodes
    without tripping the range, signature or type tests.
    """
    region = bytearray(fixture())
    struct.pack_into("<Q", region, index * NODE_SIZE + SIZE_FIELD,
                     IMAGE_TOTAL * 2)

    return bytes(region)


IMAGE_CASES = [
    ("a clean fixture image", "img-fixture", fixture(), None, True, ""),
    ("an undeclared set-id node", "img-fixture",
     fixture(3, (NODE_RESERVED, 13, b"busybox", 0o4755, 0, 0, 11)), None,
     False, "does not declare it"),
    ("a declared node stripped of its bit", "img-fixture",
     fixture(2, (NODE_RESERVED, 12, b"su", 0o755, 0, 0, 11)), None,
     False, "no such set-id node"),
    ("a declared node at the wrong owner", "img-fixture",
     fixture(2, (NODE_RESERVED, 12, b"su", 0o4755, 5, 0, 11)), None,
     False, "allowlist declares 04755:0:0"),
    ("the sentinel at the wrong mode", "img-fixture",
     fixture(4, (NODE_RESERVED, 14, b"hello.txt", 0o600, 0, 0, 0)), None,
     False, "not the declared"),
    ("the sentinel missing", "img-fixture",
     fixture(4, (NODE_RESERVED, 14, b"other.txt", 0o644, 0, 0, 0)), None,
     False, "the sentinel /hello.txt"),
    ("an unprivileged file missing", "img-fixture",
     fixture(3, (NODE_RESERVED, 13, b"other", 0o755, 0, 0, 11)), None,
     False, "present and unprivileged"),
    ("an unprivileged file at a mode nobody declared", "img-fixture",
     fixture(3, (NODE_RESERVED, 13, b"busybox", 0o777, 0, 0, 11)), None,
     False, "trust boundaries too"),
    ("the directory holding them writable by anyone", "img-fixture",
     fixture(1, (NODE_RESERVED, 11, b"bin", 0o40777, 0, 0, 0)), None,
     False, "may write is a way to replace"),
    ("the directory holding them owned by somebody else", "img-fixture",
     fixture(1, (NODE_RESERVED, 11, b"bin", 0o40755, 1000, 0, 0)), None,
     False, "may write is a way to replace"),
    ("a region cut on a node boundary", "img-fixture", fixture(),
     NODE_SIZE * 3, False, "does not close"),
    ("a node larger than the region", "img-fixture", oversized_fixture(2),
     None, False, "which no node"),
    ("two nodes on one path", "img-fixture",
     fixture(3, (NODE_RESERVED, 13, b"su", 0o755, 0, 0, 11)), None,
     False, "resolve to the path"),
    ("a non-ASCII node name", "img-fixture",
     fixture(3, (NODE_RESERVED, 13, b"m\xff", 0o755, 0, 0, 11)), None,
     False, "not ASCII"),
    ("an allowlist with nothing to find", "img-blind", fixture(), None,
     False, "nothing to find"),
]


def run_case(name, got, want_ok, fragment):
    """Judge one case's findings. Returns the number of failures."""
    if want_ok and got:
        print("  FAIL: %s: expected a clean run, got: %s" % (name, got[0]))

        return 1
    if not want_ok and not got:
        print("  FAIL: %s: expected a finding, the run was clean" % name)

        return 1
    if not want_ok and not [p for p in got if fragment in p]:
        print("  FAIL: %s: no finding named '%s'; got: %s"
              % (name, fragment, "; ".join(got)))

        return 1

    return 0


def declared_init_line(stem, table=ALLOWLIST):
    """The /sbin/init line a manifest with this stem must carry, or ""."""
    want = table.get(stem, {}).get("init")

    return "" if want is None else ("file 0755 /sbin/init build/utils/sbin/%s\n" % want)


# The init check's own cases. The set-id fixtures above carry their declared
# init line for free, so here the line is exactly what varies: a manifest that
# stages the other artifact, one that stages it from somewhere else, one that
# makes it optional, one that stages none, and one that stages an init where the
# allowlist declares none.
INIT_CASES = [
    ("the declared init", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0755 /sbin/init build/utils/sbin/init-login\n", True, ""),
    ("the other init", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0755 /sbin/init build/utils/sbin/init-shell\n", False,
     "build/utils/sbin/init-login"),
    ("the declared init from somewhere else", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0755 /sbin/init build/elsewhere/init-login\n", False,
     "build/utils/sbin/init-login"),
    ("an optional init", "os-simple",
     OS_SIMPLE_MINIMUM + "optional-file 0755 /sbin/init build/utils/sbin/init-login\n", False,
     "must be 'file'"),
    ("the declared init staged writable by anyone", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0777 /sbin/init build/utils/sbin/init-login\n",
     False, "must be 0755:0:0"),
    ("the declared init staged to another owner", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0755:1000:0 /sbin/init build/utils/sbin/init-login\n",
     False, "must be 0755:0:0"),
    ("an inline init", "os-simple",
     OS_SIMPLE_MINIMUM + "text 0755 /sbin/init placeholder\n", False, "an inline payload"),
    ("no init at all", "os-simple",
     OS_SIMPLE_MINIMUM + "file 0755 /hello.txt build/utils/cat\n", False, "stages no /sbin/init"),
    ("an init where none is declared", "test",
     TEST_MINIMUM + "file 0755 /sbin/init build/utils/sbin/init-login\n", False,
     "declares none for this image"),
]


#
# The set-id config's fixtures. Each is the config the set-id binary was built
# from with one setting turned the wrong way, so what they prove is that the
# guard reads the setting rather than the file's name. Two of them are not
# settings a person would write but shapes a hand-edited file can have, where
# the question is whose reading wins, Kconfig's or this guard's: the answer is
# that a file with either shape is refused rather than read. The clean case
# reads the file that was built, which is the sentinel: if the config were
# regenerated under other symbol names, every mutation below would still fail
# and this one would say so.
def suid_config_cases(tmp):
    """Yield (name, path, want_ok, fragment) over mutations of the real config."""
    try:
        with open(SUID_BUSYBOX_CONFIG, "r", encoding="latin-1",
                  newline="\n") as handle:
            text = handle.read()
    except OSError as error:
        yield ("the set-id config as it ships", SUID_BUSYBOX_CONFIG, False,
               str(error))

        return

    yield ("the set-id config as it ships", SUID_BUSYBOX_CONFIG, True, None)

    mutations = (
        ("a symbol decided twice, the dangerous one first",
         "# CONFIG_FEATURE_SUID_CONFIG is not set",
         "CONFIG_FEATURE_SUID_CONFIG=y\n# CONFIG_FEATURE_SUID_CONFIG is not set",
         "decided 2 times"),
        ("a value this guard does not read",
         "# CONFIG_FEATURE_SUID_CONFIG is not set",
         "CONFIG_FEATURE_SUID_CONFIG=yes", "does not read"),
        ("the suid.conf parser compiled back in",
         "# CONFIG_FEATURE_SUID_CONFIG is not set",
         "CONFIG_FEATURE_SUID_CONFIG=y", "capability"),
        ("a shell smuggled into the set-id binary",
         "# CONFIG_SH_IS_ASH is not set", "CONFIG_SH_IS_ASH=y", "capability"),
        ("applets preferred over the programs on disk",
         "# CONFIG_FEATURE_PREFER_APPLETS is not set",
         "CONFIG_FEATURE_PREFER_APPLETS=y", "capability"),
        ("the set-id bit no longer honored",
         "CONFIG_FEATURE_SUID=y", "# CONFIG_FEATURE_SUID is not set",
         "is off"),
        ("a symbol the guard names gone from the config",
         "CONFIG_SH_IS_NONE=y", "", "proved nothing"),
    )
    for i, (name, old, new, fragment) in enumerate(mutations):
        if old not in text:
            yield (name, SUID_BUSYBOX_CONFIG, False,
                   "the fixture's own line is not in the config")
            continue
        path = os.path.join(tmp, "suid-%d.config" % i)
        with open(path, "w", encoding="latin-1", newline="\n") as handle:
            handle.write(text.replace(old, new, 1))
        yield (name, path, False, fragment)


def selftest():
    """Run both halves over the fixtures. Returns a process status."""
    bad = 0
    ran = 0

    for problem in validate_table():
        print("  FAIL: the allowlist itself: %s" % problem)
        bad += 1

    with tempfile.TemporaryDirectory() as tmp:
        for name, stem, text, want_ok, fragment in SELFTEST_CASES:
            path = os.path.join(tmp, stem + ".manifest")
            with open(path, "w", encoding="latin-1", newline="\n") as f:
                # Each fixture is about the set-id sweep, so it gets the init
                # line its stem declares for free; the init check has its own
                # cases below, where the line is what varies.
                f.write(declared_init_line(stem) + text)
            bad += run_case(name, check_manifest(path), want_ok, fragment)
            ran += 1

        for name, stem, text, want_ok, fragment in INIT_CASES:
            path = os.path.join(tmp, stem + ".manifest")
            with open(path, "w", encoding="latin-1", newline="\n") as f:
                f.write(text)
            bad += run_case(name, check_manifest(path), want_ok, fragment)
            ran += 1

        for name, path, want_ok, fragment in suid_config_cases(tmp):
            bad += run_case(name, check_suid_config(path), want_ok, fragment)
            ran += 1

        for name, table, fragment in TABLE_CASES:
            bad += run_case(name, validate_table(table), False, fragment)
            ran += 1

        missing = os.path.join(tmp, "os-real.manifest")
        problems, _ = sweep([missing], ALLOWLIST, require_all=True)
        bad += run_case("a manifest the table names but the tree lacks",
                        problems, False, "which is not in")
        ran += 1

        for name, stem, region, size, want_ok, fragment in IMAGE_CASES:
            image = os.path.join(tmp, stem + ".krffs")
            with open(image, "wb") as f:
                f.write(region)
            manifest = os.path.join(tmp, stem + ".manifest")
            problems, _ = check_image(image, manifest, 0, size, IMAGE_TABLE)
            bad += run_case(name, problems, want_ok, fragment)
            ran += 1

    if bad:
        print("check-setid: selftest: %d of %d cases failed" % (bad, ran))

        return 1

    print("check-setid: selftest: %d cases PASS" % ran)

    return 0


def report(problems):
    """Print the findings. Returns a process status."""
    sys.stderr.write("check-setid: FAIL\n")
    for problem in problems:
        sys.stderr.write("  - " + problem + "\n")

    return 1



# What the MAIN busybox must not carry. Two reasons, and the list says which is
# which. The first six want the set-id bit, and this binary is staged without
# one, so an applet of that name would answer from the shell's PATH before the
# set-id build's copy and then refuse for want of a bit it does not have. The
# last five need a raw socket, which this kernel's socket layer does not offer,
# so they cannot work at all. `getty` and `sulogin` are NOT here: both are
# BB_SUID_DROP and neither wants privilege; they are simply not selected,
# because this system's init does not use them.
#
# The sentinel is an applet that must be present, because a check that only ever
# looks for absences passes just as well on a header it failed to understand.
FORBIDDEN_APPLETS = ("su", "passwd", "login", "vlock", "wall", "crontab",
                     "ping", "ping6", "traceroute", "traceroute6", "arping")
APPLET_SENTINEL = "cat"

# The applet table as it was STAGED, beside the binary it describes, rather than
# the copy generated inside the extracted source tree. `build_busybox.sh` opens
# with `rm -rf` and a fresh `tar x`, so the in-tree file is absent for most of a
# rebuild and describes the previous configuration in the window before the
# re-extract; the staged copy describes what actually ships.
APPLET_TABLE = os.path.join(
    "build", "vendor", "busybox", "applet_tables-main.h")


def applet_names(path):
    """Read a generated applet table. Returns (names, problems).

    The names live in one NUL-delimited string, so the match is on whole tokens
    rather than on a substring: `passwd` appears inside `passwd_main` and inside
    a help string, and a search that did not split would find it in a busybox
    that compiled the applet out. A missing or empty table is a problem rather
    than an empty answer, so an unreadable scan fails instead of reading as a
    clean absence.
    """
    if not os.path.exists(path):
        return set(), ["%s: no applet table; build busybox before checking it" % path]
    try:
        with open(path, "r", encoding="latin-1", newline="\n") as handle:
            text = handle.read()
    except OSError as error:
        return set(), ["%s: cannot read the applet table: %s" % (path, error)]

    # The whole initializer or nothing. A file truncated part way through it
    # would otherwise read as a shorter applet set, and the names are sorted, so
    # a prefix that still holds the `cat` sentinel can have lost `passwd` and
    # `su` from the tail: the guard would find no forbidden applet because it
    # never read as far as one. The declared count is the other half of that,
    # since a file can end in the right punctuation and still be short.
    start = text.find("const char applet_names[]")
    if start < 0:
        return set(), ["%s: no applet_names table; this is not a busybox applet "
                       "table, so the check proved nothing" % path]
    end = text.find(";", start)
    if end < 0:
        return set(), ["%s: the applet_names table is unterminated, so it was "
                       "read part way and proves nothing" % path]

    names = set()
    tokens = 0
    for chunk in re.findall(r'"((?:[^"\\]|\\.)*)"', text[start:end]):
        for token in chunk.split("\\0"):
            token = token.strip()
            if token:
                names.add(token)
                tokens += 1

    declared = re.search(r"#define\s+NUM_APPLETS\s+(\d+)", text)
    if not declared:
        return set(), ["%s: the table declares no NUM_APPLETS, so its length "
                       "cannot be checked" % path]
    if tokens != int(declared.group(1)):
        return set(), ["%s: the table names %d applets but declares %s; it was "
                       "truncated or edited" % (path, tokens, declared.group(1))]

    return names, []


def check_applets(path=APPLET_TABLE):
    """Check a generated busybox applet table. Returns a list of problems.

    The names live in one NUL-delimited string, so the match is on whole tokens
    rather than on a substring: `passwd` appears inside `passwd_main` and inside
    a help string, and a search that did not split would find it in a busybox
    that compiled the applet out.
    """
    names, problems = applet_names(path)
    if problems:
        return problems

    if APPLET_SENTINEL not in names:
        problems.append(
            "%s: the applet table does not name `%s`, so this check found "
            "nothing rather than proving an absence" % (path, APPLET_SENTINEL))
        return problems
    for applet in FORBIDDEN_APPLETS:
        if applet in names:
            problems.append(
                "%s: busybox carries the `%s` applet; privilege climbs through "
                "the declared set-id programs alone" % (path, applet))

    return problems



# The set-id build's table, and the rule that points the other way. Everything in
# the main binary is unprivileged, so its rule is a blacklist: no applet that
# wants the set-id bit and none that needs a raw socket. Everything in THIS
# binary gets the bit, so its rule has to be a whitelist: exactly these three and
# nothing else. A blacklist here would pass a binary that had quietly grown a
# shell, which is the one thing a set-id executable must never contain, and
# `allnoconfig` leaves `CONFIG_SH_IS_ASH` set, so that is a mistake a config
# written by omission actually makes.
SUID_APPLET_TABLE = os.path.join(
    "build", "vendor", "busybox", "applet_tables-suid.h")
SUID_APPLETS = ("login", "passwd", "su")


def check_suid_applets(path=SUID_APPLET_TABLE):
    """Check the set-id build's applet table. Returns a list of problems."""
    names, problems = applet_names(path)
    if problems:
        return problems

    extra = sorted(names - set(SUID_APPLETS))
    missing = sorted(set(SUID_APPLETS) - names)
    if extra:
        problems.append(
            "%s: the set-id busybox carries %s beside the declared %s; every "
            "applet in that binary runs as root"
            % (path, ", ".join("`%s`" % n for n in extra),
               ", ".join("`%s`" % n for n in SUID_APPLETS)))
    if missing:
        problems.append(
            "%s: the set-id busybox is missing %s, so this check proved nothing "
            "about the binary that ships"
            % (path, ", ".join("`%s`" % n for n in missing)))

    return problems


# Config symbols a shipped busybox must not carry, checked in the source config
# rather than in the generated table because they are features inside an applet
# and no applet name changes when they come back.
#
# `FEATURE_HTTPD_AUTH_MD5` is one: `httpd -m` mints a `$1$` hash and prints it,
# through the same pid-and-clock salt `cryptpw` used, and standalone ash reaches
# the applet with no file to remove. A hash minted that way is accepted by
# `pwauth_check` and by dropbear once it is in `/etc/shadow`, so leaving it
# enabled would keep a second minter that cannot fail closed, which is the thing
# the entropy rule exists to prevent.
# Nothing is forbidden here now. `CONFIG_FEATURE_HTTPD_AUTH_MD5` was, because
# `httpd -m` mints a password hash from a salt made of a pid and a clock, and
# this system minted salts one way, from the kernel's CSPRNG, refusing when that
# generator is not trusted. That objection was retired with the same judgment
# that brought `cryptpw` and `mkpasswd` back: busybox's salt is weak, its own
# comment says so, and for a teaching machine whose threat model is a student it
# is good enough. The rule and its self-tests are kept rather than deleted,
# because the next feature that returns a capability the applet names do not
# mention will want them.
FORBIDDEN_CONFIG = ()

# The config each build actually used, staged beside its binary by
# build_busybox.sh, and not the hand-written file in vendor/busybox. The reason
# is the applet table's reason and one more. `oldconfig` resolves what a person
# wrote into one canonical line per symbol, so a guard reading the resolved file
# needs none of Kconfig's rules for a symbol assigned twice, or written `=yes`,
# or left out for a default to fill in: those are decided before this reads
# anything, and what it reads is what was compiled.
BUSYBOX_CONFIG = os.path.join("build", "vendor", "busybox", "config-main")


# The set-id build's config, and the settings that make it the thing its name
# claims. The whitelist above reads the generated table, which holds names and
# nothing else, so every one of these is invisible to it: turning
# `FEATURE_SUID_CONFIG` back on leaves exactly `login`, `passwd` and `su` in the
# table while compiling in a parser of a file the binary reads as root, and one
# with a use-after-free at that. A shell coming back is the same kind of
# invisible, since a shell reached through `sh` is a name the table does carry
# but the whitelist would then have to be widened to admit.
#
# Both halves are required to APPEAR, on or off, for the reason the applet scan
# has a sentinel: a config that was regenerated by a busybox version using other
# symbol names would otherwise pass every check by holding none of them.
SUID_BUSYBOX_CONFIG = os.path.join("build", "vendor", "busybox", "config-suid")
SUID_CONFIG_ON = ("CONFIG_STATIC", "CONFIG_LFS", "CONFIG_FEATURE_SUID",
                  "CONFIG_FEATURE_SHADOWPASSWDS", "CONFIG_SH_IS_NONE",
                  "CONFIG_LOGIN", "CONFIG_PASSWD", "CONFIG_SU")
SUID_CONFIG_OFF = ("CONFIG_FEATURE_SUID_CONFIG", "CONFIG_FEATURE_PREFER_APPLETS",
                   "CONFIG_LOGIN_SCRIPTS", "CONFIG_BUSYBOX", "CONFIG_ASH",
                   "CONFIG_HUSH", "CONFIG_SH_IS_ASH", "CONFIG_SH_IS_HUSH",
                   "CONFIG_FEATURE_SH_STANDALONE")

# Why each of the ones that are off:
#
# `FEATURE_SUID_CONFIG` reads /etc/busybox.conf as root and frees the parsed
# table while a global still points into it; that bug is upstream and this
# config's answer to it is not to compile the parser.
# `FEATURE_PREFER_APPLETS` and `FEATURE_SH_STANDALONE` make a name resolve to an
# applet inside this binary rather than to the program on disk, which is how a
# set-id process quietly reaches code it was not staged to run.
# `LOGIN_SCRIPTS` runs a program named by a variable at login.
# `BUSYBOX`, `ASH`, `HUSH` and the two `SH_IS_` selectors are the shell, which is
# the one thing a set-id executable must never contain.


def check_suid_config(path=SUID_BUSYBOX_CONFIG):
    """Check the set-id build's config. Returns a list of problems."""
    if not os.path.exists(path):
        return ["%s: no set-id busybox config to check" % path]

    try:
        with open(path, "r", encoding="latin-1", newline="\n") as handle:
            lines = handle.read().split("\n")
    except OSError as error:
        return ["%s: cannot read the config: %s" % (path, error)]

    state = {}
    seen = {}
    problems = []
    for line in lines:
        for symbol in SUID_CONFIG_ON + SUID_CONFIG_OFF:
            if line.startswith(symbol + "="):
                value = line[len(symbol) + 1:].strip()
                if value not in ("y", "n"):
                    problems.append(
                        "%s: %s is set to `%s`, which this guard does not read; "
                        "the resolved config writes y, n or a not-set comment"
                        % (path, symbol, value))
                state[symbol] = value == "y"
                seen[symbol] = seen.get(symbol, 0) + 1
            elif line == ("# %s is not set" % symbol):
                state[symbol] = False
                seen[symbol] = seen.get(symbol, 0) + 1

    # A symbol decided twice is a config that was not resolved: `oldconfig`
    # writes each one once, and two lines mean the file was hand-edited or came
    # from somewhere else, where which of the two won is Kconfig's rule and not
    # this reader's.
    for symbol in sorted(seen):
        if seen[symbol] > 1:
            problems.append(
                "%s: %s is decided %d times; a resolved config decides it once"
                % (path, symbol, seen[symbol]))

    for symbol in SUID_CONFIG_ON:
        if symbol not in state:
            problems.append(
                "%s: %s does not appear at all, so this check proved nothing "
                "about it" % (path, symbol))
        elif not state[symbol]:
            problems.append(
                "%s: %s is off; the set-id binary is then not the one this "
                "guard describes" % (path, symbol))
    for symbol in SUID_CONFIG_OFF:
        if symbol not in state:
            problems.append(
                "%s: %s does not appear at all, so this check proved nothing "
                "about it" % (path, symbol))
        elif state[symbol]:
            problems.append(
                "%s: %s is on; it gives the set-id binary a capability its "
                "three applet names do not mention" % (path, symbol))

    return problems


def check_config(path=BUSYBOX_CONFIG):
    """Check the busybox config for features that return a forbidden capability."""
    if not os.path.exists(path):
        return ["%s: no busybox config to check" % path]

    try:
        with open(path, "r", encoding="latin-1", newline="\n") as handle:
            lines = handle.read().split("\n")
    except OSError as error:
        return ["%s: cannot read the config: %s" % (path, error)]

    problems = []
    seen = 0
    for line in lines:
        for symbol in FORBIDDEN_CONFIG:
            if line.startswith(symbol + "=") or line == ("# %s is not set" % symbol):
                seen += 1
            if line.startswith(symbol + "=y"):
                problems.append(
                    "%s: %s is set; it mints a password hash from a salt no entropy "
                    "source was read for" % (path, symbol))
    if seen < len(FORBIDDEN_CONFIG):
        problems.append(
            "%s: not every forbidden symbol appears at all, so this check proved "
            "nothing; the config may have been regenerated" % path)

    return problems


# The two init artifacts, and the byte strings that tell them apart. init-login
# execs /bin/login and nothing else; the shell table and the loop that walks it
# live inside an `#ifndef`, so the strict artifact holds no code path and no
# string that names a shell. Requiring init-shell to carry BOTH is the sentinel:
# an unreadable or empty scan then fails rather than passes, the way the applet
# table's own scan is guarded. The check is exact in this tree because a static
# musl util that never calls `system` carries neither string; only init's own
# table puts them in a binary.
INIT_BINARIES = os.path.join("build", "utils", "sbin")
INIT_SHELL_STRINGS = (b"/bin/sh", b"/ish")


def check_inits(directory=INIT_BINARIES):
    """Check that the strict init cannot name a shell and the other can."""
    problems = []
    wanted = {"init-login": False, "init-shell": True}

    for name in sorted(wanted):
        path = os.path.join(directory, name)
        try:
            with open(path, "rb") as handle:
                image = handle.read()
        except OSError as error:
            problems.append("%s: cannot read the init artifact: %s"
                            % (path, error))
            continue
        if not image:
            problems.append("%s: the init artifact is empty, so this check "
                            "proved nothing" % path)
            continue
        for wanted_string in INIT_SHELL_STRINGS:
            held = wanted_string in image
            if wanted[name] and not held:
                problems.append(
                    "%s: does not carry the byte string %s, so this check "
                    "proved nothing; the artifact may not be the one the "
                    "manifests stage"
                    % (path, wanted_string.decode("ascii")))
            elif not wanted[name] and held:
                problems.append(
                    "%s: carries the byte string %s; the strict init must hold "
                    "no path it could fall back to, since a fall-through on an "
                    "exec failure hands the console an unauthenticated root "
                    "shell" % (path, wanted_string.decode("ascii")))

    return problems


def main(argv):
    parser = argparse.ArgumentParser(
        description="the set-id guard for the manifests and the staged images")
    parser.add_argument("manifests", nargs="*", metavar="MANIFEST",
                        help="manifests to sweep (all of them by default)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the sweep's own cases over fixtures")
    parser.add_argument("--applets", nargs="?", const=APPLET_TABLE,
                        help="check a generated busybox applet table")
    parser.add_argument("--image", help="staged image to inventory")
    parser.add_argument("--manifest", help="the manifest that staged --image")
    parser.add_argument("--offset", default="0",
                        help="byte offset of the file system in the image")
    parser.add_argument("--size", default=None,
                        help="byte size of the file system in the image")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.applets:
        problems = (check_applets(args.applets) + check_suid_applets()
                    + check_config() + check_suid_config() + check_inits())
        if problems:
            return report(problems)
        print("check-setid: OK (%s: no forbidden applet or feature; the set-id build "
              "holds only %s; the strict init names no shell)"
              % (args.applets, ", ".join(SUID_APPLETS)))
        return 0

    if args.image:
        if not args.manifest:
            sys.stderr.write("check-setid: --image needs the --manifest that "
                             "staged it\n")
            return 2
        if args.manifests:
            sys.stderr.write("check-setid: --image inventories one image "
                             "against one --manifest; it does not sweep the "
                             "manifests named beside it\n")
            return 2
        try:
            offset = int(args.offset, 0)
            size = int(args.size, 0) if args.size is not None else None
        except ValueError:
            sys.stderr.write("check-setid: --offset and --size take a "
                             "number\n")
            return 2
        if offset != 0 and size is None:
            sys.stderr.write("check-setid: --offset needs --size; a file "
                             "system inside a partition ends where the "
                             "partition does, not where the file does\n")
            return 2
        problems, summary = check_image(args.image, args.manifest, offset,
                                        size)
        if problems:
            return report(problems)
        print("check-setid: OK (%s, every set-id node declared)" % summary)
        return 0

    if args.manifest:
        sys.stderr.write("check-setid: --manifest names the manifest of an "
                         "--image; the sweep takes its manifests as "
                         "arguments\n")
        return 2

    paths = args.manifests
    require_all = not paths
    if require_all:
        paths = sorted(glob.glob(os.path.join(MANIFEST_DIR, "*.manifest")))
        if not paths:
            sys.stderr.write("check-setid: no manifests in %s\n"
                             % MANIFEST_DIR)
            return 2

    problems, count = sweep(paths, require_all=require_all)
    if problems:
        return report(problems)

    entries = sum(len(ALLOWLIST[manifest_stem(p)]["setid"]) for p in paths)
    print("check-setid: OK (%d manifests swept, %d declared set-id entries "
          "staged)" % (count, entries))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
