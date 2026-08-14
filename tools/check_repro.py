#!/usr/bin/env python3
"""
check_repro.py: one tree, one image.

A staged image is built from a manifest and a set of host artifacts, and nothing
about the machine that built it may show through. Two things used to. `mkfs`
stamps the root node with the building host's clock, at microsecond resolution,
so two builds a moment apart differed in those bytes. And `exec-dir` staged
whatever order `readdir` handed it, which is the host file system's order: on
this one it is a hash of the names, on another it is creation order, and on a
third it is neither.

The order half needs care to test at all. Staging one directory twice cannot
see the bug, since one directory returns one order; and creating the same names
in a different sequence does not help on a host that orders by hash. So what is
asserted is the property the sort provides: the names land in the image in NAME
order. That is only a test where the host's own order differs from sorted, so
this checks that first and says it could not test rather than passing quietly.

Usage:
    check_repro.py [mkfs.krffs] [stageimage.krffs]
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_setid

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Candidate name sets. The point of having several is that a host reports its
# directory entries in an order of its own (a hash of the names here, creation
# order elsewhere), and this check can only see an unsorted stager on a set
# whose host order differs from name order. So it tries sets until it finds one
# that does, and says so plainly when none does.
#
# Every set mixes case on purpose: under `strcmp` every capital sorts before
# every lower-case letter, and under a case-folding comparator they interleave,
# so a stager that sorted with the wrong one is caught rather than accepted.
NAME_SETS = (
    ("delta", "Foxtrot", "charlie", "Alpha", "echobox", "bravo"),
    ("Zulu", "yankee", "Xray", "whiskey", "Victor", "uniform"),
    ("mike", "November", "oscar", "Papa", "quebec", "Romeo"),
    ("Golf", "hotel", "India", "juliet", "Kilo", "lima"),
)


def run(argv):
    """Run a build tool, failing loudly rather than leaving a half-made image."""
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("check-repro: %s failed: %s"
                         % (argv[0], result.stderr.strip()))


def stage(mkfs, stager, manifest, image):
    """Make a fresh file system in `image` and stage `manifest` into it."""
    with open(image, "wb") as handle:
        handle.write(b"\0" * (1024 * 1024))
    run([mkfs, image])
    run([stager, manifest, image])


def staged_names(image, guestdir):
    """The names under `guestdir`, in the order the image holds them."""
    region = check_setid.read_region(image, 0, None)
    nodes = check_setid.walk_nodes(region)
    paths = check_setid.node_paths(nodes)
    ordered = []
    for node in nodes:
        if node["type"] != check_setid.NODE_RESERVED:
            continue
        for path, other in paths.items():
            if other is node and path.startswith(guestdir + "/"):
                ordered.append(path[len(guestdir) + 1:])
    return ordered


def main(argv):
    mkfs = argv[0] if argv else os.path.join(REPO, "build/krffs/mkfs.krffs")
    stager = argv[1] if len(argv) > 1 else os.path.join(REPO, "build/krffs/stageimage.krffs")
    for tool in (mkfs, stager):
        if not os.access(tool, os.X_OK):
            raise SystemExit("check-repro: %s is missing; run make krffs" % tool)

    work = tempfile.mkdtemp(prefix="rach2_repro.")
    try:
        artifacts = os.path.join(work, "bin")
        names = None
        scrambled = False
        for candidate in NAME_SETS:
            shutil.rmtree(artifacts, ignore_errors=True)
            os.mkdir(artifacts)
            for name in candidate:
                path = os.path.join(artifacts, name)
                with open(path, "w") as handle:
                    handle.write("contents of %s\n" % name)
                os.chmod(path, 0o755)
            names = candidate
            host_order = os.listdir(artifacts)
            if host_order != sorted(host_order):
                scrambled = True
                break

        manifest = os.path.join(work, "m.manifest")
        with open(manifest, "w") as handle:
            handle.write("dir 0755 /bin\n")
            handle.write("exec-dir 0755 /bin %s\n" % artifacts)

        first = os.path.join(work, "one.img")
        second = os.path.join(work, "two.img")
        stage(mkfs, stager, manifest, first)
        stage(mkfs, stager, manifest, second)

        # The clock half. mkfs stamps the root with the host's time of day at
        # microsecond resolution, so two stagings are never in the same instant
        # and no sleep is needed to tell them apart.
        with open(first, "rb") as a, open(second, "rb") as b:
            if a.read() != b.read():
                print("check-repro: FAIL: one manifest staged twice produced two "
                      "different images")
                print("check-repro: the root node's timestamps are the usual reason")
                return 1

        # The order half. The expectation is the EXACT list, not merely a
        # sorted one: a stager that dropped an entry leaves a list that is still
        # in order, and a stager that folded case leaves one that is in a
        # different order than this comparator's.
        image_order = staged_names(first, "/bin")
        if image_order != sorted(names):
            print("check-repro: FAIL: the staged names are %s, and name order is %s"
                  % (", ".join(image_order) or "(none)", ", ".join(sorted(names))))
            print("check-repro: exec-dir must sort its entries the way tree does")
            return 1

        if not scrambled:
            # Every set this check knows came back already in name order on this
            # host, so the assertion above held without the stager having to
            # sort anything. Said out loud rather than reported as a pass: the
            # clock half was tested, the ordering half was not.
            print("check-repro: OK (one tree, one image; the clock excluded). "
                  "This host reports directory entries in name order, so the "
                  "ordering half was not exercised")
            return 0

        print("check-repro: OK (one tree, one image; the clock and the host's "
              "directory order both excluded)")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
