#!/usr/bin/env python3
"""
check_setid.py: the guard on the set-id bits of the staged images.

Privilege in a Rach2 image climbs through a few declared files and nowhere
else: os-real has /bin/su, the test image has the suidprobe and sudoprobe
fixtures, and the other images have none. The kernel honors the set-id bits at
exec, so a bit staged anywhere else is a real privilege hole; the bits are
declared in the table below before the build may stage them.

The guard has two halves, and they answer different questions.

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
    check_setid.py --selftest
        Run both halves over the fixture manifests and images held in this
        script.

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
# and hands a setuid-root busybox to the image. `unprivileged` names files that
# must be staged and must carry no set-id bit. `sentinel` names one ordinary
# node the image inventory must report exactly; it is required of a manifest
# that declares no set-id entry, which would otherwise have nothing positive to
# find and would read a misparsed image as clean.
#
ALLOWLIST = {
    "os-real": {
        # Privilege climbs through /bin/su alone: a small setuid-root program
        # (utils/su.c). busybox stays unprivileged, since a setuid busybox
        # would run every one of its BB_SUID_REQUIRE and BB_SUID_MAYBE applets
        # as root unless an /etc/busybox.conf confined it, and that config path
        # trips a busybox use-after-free the kernel's strict munmap faults.
        "setid": {"/bin/su": (0o4755, 0, 0, "build/utils/sbin/su")},
        "unprivileged": ("/bin/busybox",),
        "sentinel": None,
    },
    "test": {
        # suidprobe's fixtures (the same helper at several modes and owners),
        # sudoprobe's reference sudo, and suidmusl's setuid copy, which proves
        # libc's view of a set-id exec.
        "setid": {
            "/suidhelper.suid": (0o4755, 0, 0, "build/tests/suidhelper"),
            "/suidhelper.u5": (0o4755, 5, 0, "build/tests/suidhelper"),
            "/suidhelper.sgid": (0o2755, 0, 5, "build/tests/suidhelper"),
            "/suidhelper.nox": (0o4644, 0, 0, "build/tests/suidhelper"),
            "/sudo": (0o4755, 0, 0, "build/utils/sbin/sudo"),
            "/musl/suidmusl.suid": (0o4755, 0, 0,
                                    "build/tests/musl/suidmusl"),
        },
        "unprivileged": (),
        "sentinel": None,
    },
    "os-simple": {
        "setid": {},
        "unprivileged": (),
        "sentinel": ("/hello.txt", 0o644, 0, 0),
    },
    "disk-test": {
        "setid": {},
        "unprivileged": (),
        "sentinel": ("/blockprobe", 0o755, 0, 0),
    },
    "persist-test": {
        "setid": {},
        "unprivileged": (),
        "sentinel": ("/persistprobe", 0o755, 0, 0),
    },
    "stress-test": {
        "setid": {},
        "unprivileged": (),
        "sentinel": ("/stressprobe", 0o755, 0, 0),
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

    for guest in allow["unprivileged"]:
        if guest not in staged:
            problems.append("%s: %s is not staged; the allowlist requires it "
                            "to be present and unprivileged" % (name, guest))
            continue
        lineno, mode, uid, gid, source = staged[guest]
        if mode & SETID:
            problems.append("%s:%d: %s is staged set-id (%s); it must stay "
                            "unprivileged, and privilege must climb through "
                            "the declared paths alone"
                            % (name, lineno, guest, fmt_entry(mode, uid, gid)))

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

    for path in allow["unprivileged"]:
        node = paths.get(path)
        if node is None:
            problems.append("%s: %s is not in the image; the allowlist "
                            "requires it to be present and unprivileged"
                            % (os.path.basename(image), path))
        elif node["mode"] & SETID:
            problems.append("%s: %s is staged set-id (%s); it must stay "
                            "unprivileged"
                            % (os.path.basename(image), path,
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
OS_REAL_MINIMUM = ("file 0755 /bin/busybox build/vendor/busybox/busybox\n"
                   "file 04755:0:0 /bin/su build/utils/sbin/su\n")

TEST_MINIMUM = ("file 04755:0:0 /suidhelper.suid build/tests/suidhelper\n"
                "file 04755:5:0 /suidhelper.u5 build/tests/suidhelper\n"
                "file 02755:0:5 /suidhelper.sgid build/tests/suidhelper\n"
                "file 04644:0:0 /suidhelper.nox build/tests/suidhelper\n"
                "file 04755:0:0 /sudo build/utils/sbin/sudo\n"
                "file 04755:0:0 /musl/suidmusl.suid "
                "build/tests/musl/suidmusl\n")

# A line of exactly MAX_LINE content bytes, the longest stageimage accepts.
LONGEST_LINE = ("file 0755 /pad build/utils/cat"
                + " " * (MAX_LINE - len("file 0755 /pad build/utils/cat")))

SELFTEST_CASES = [
    ("a declared set-id file", "os-real", OS_REAL_MINIMUM, True, ""),
    ("a declared set-id optional-file", "os-real",
     "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
     "optional-file 04755:0:0 /bin/su build/utils/sbin/su\n", True, ""),
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
    ("the test image's six declared entries", "test", TEST_MINIMUM, True, ""),
    ("a set-id exec-dir", "os-simple",
     "text 0644 /hello.txt hello, world\\n\n"
     "exec-dir 04755 / build/utils\n", False, "refused"),
    ("a plain exec-dir", "os-simple",
     "text 0644 /hello.txt hello, world\\n\n"
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
     "text 0644 /hello.txt\n", True, ""),
    ("a full-line comment", "os-real",
     "# file 04755:0:0 /bin/evil build/evil\n"
     "   # an indented comment line\n" + OS_REAL_MINIMUM, True, ""),
    ("a trailing hash is not a comment", "os-real",
     OS_REAL_MINIMUM + "file 04755:0:0 /bin/evil build/evil # note\n",
     False, "/bin/evil"),
    ("tab-separated fields", "os-real",
     "file\t0755\t/bin/busybox\tbuild/vendor/busybox/busybox\n"
     "file\t04755:0:0\t/bin/su\tbuild/utils/sbin/su\n", True, ""),
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
    ("the longest line stageimage accepts", "os-simple",
     "text 0644 /hello.txt hi\n" + LONGEST_LINE + "\n", True, ""),
    ("a line one byte too long", "os-simple",
     "text 0644 /hello.txt hi\n" + LONGEST_LINE + "x\n",
     False, "longer than"),
    ("the longest line ended with CRLF", "os-simple",
     "text 0644 /hello.txt hi\n" + LONGEST_LINE + "\r\n",
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
     {"x": {"setid": {}, "unprivileged": (), "sentinel": None}},
     "nothing to find"),
    ("a declared entry carrying no set-id bit",
     {"x": {"setid": {"/x": (0o755, 0, 0, "build/x")}, "unprivileged": (),
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
        "unprivileged": ("/bin/busybox",),
        "sentinel": ("/hello.txt", 0o644, 0, 0),
    },
    "img-blind": {"setid": {}, "unprivileged": (), "sentinel": None},
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
                f.write(text)
            bad += run_case(name, check_manifest(path), want_ok, fragment)
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


def main(argv):
    parser = argparse.ArgumentParser(
        description="the set-id guard for the manifests and the staged images")
    parser.add_argument("manifests", nargs="*", metavar="MANIFEST",
                        help="manifests to sweep (all of them by default)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the sweep's own cases over fixtures")
    parser.add_argument("--image", help="staged image to inventory")
    parser.add_argument("--manifest", help="the manifest that staged --image")
    parser.add_argument("--offset", default="0",
                        help="byte offset of the file system in the image")
    parser.add_argument("--size", default=None,
                        help="byte size of the file system in the image")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

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
