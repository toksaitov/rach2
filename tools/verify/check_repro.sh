#!/usr/bin/env sh

#
# check_repro.sh: one tree, one image.
#
# A staged image is built from a manifest and a set of host artifacts, and
# nothing about the machine that built it may show through. Two things used
# to. `mkfs` stamped the root node with the building host's clock, so two
# builds a moment apart differed in those bytes. And `exec-dir` staged
# whatever order `readdir` handed it, which is the host file system's
# order: a hash of the names here, creation order elsewhere.
#
# The order half needs care to test at all. Staging one directory twice
# cannot see the bug, since one directory returns one order. So what is
# asserted is the property the sort provides: the names land in the image
# in NAME order. That is only a test where the host's own order differs
# from sorted, so this tries name sets until one does and says it could
# not test instead of passing quietly. Every set mixes case on purpose:
# under strcmp every capital sorts before every lower-case letter, so a
# stager that sorted with a case-folding comparator is caught too.
#
# The node inventory comes from `setidcheck --nodes`, which prints each
# root or reserved node in ring order with its path and timestamps.
#
# Usage: check_repro.sh [mkfs.krffs] [stageimage.krffs]
#

set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MKFS="${1:-$REPO/build/krffs/mkfs.krffs}"
STAGER="${2:-$REPO/build/krffs/stageimage.krffs}"

for tool in "$MKFS" "$STAGER"; do
    [ -x "$tool" ] || {
        echo "check-repro: $tool is missing; run make krffs" >&2
        exit 1
    }
done

SETID="$REPO/build/tools/inspect/setidcheck"
if [ ! -x "$SETID" ] || [ "$REPO/tools/inspect/setidcheck.c" -nt "$SETID" ]; then
    mkdir -p "${SETID%/*}"
    cc -O2 -o "$SETID" "$REPO/tools/inspect/setidcheck.c" || exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/rach2_repro.XXXXXX")"
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf "$WORK"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'rm -rf "$WORK"' INT TERM

fail() {
    echo "check-repro: FAIL: $1"
    [ -z "${2:-}" ] || echo "check-repro: $2"
    exit 1
}

stage() {
    dd if=/dev/zero of="$2" bs=1048576 count=1 2>/dev/null
    "$MKFS" "$2" >/dev/null || fail "$MKFS failed"
    "$STAGER" "$1" "$2" >/dev/null || fail "$STAGER failed"
}

# The names under a guest directory, in the order the image has them.
staged_names() {
    "$SETID" --nodes --image "$1" | awk -v dir="$2/" '
        $1 == "reserved" && index($2, dir) == 1 {
            print substr($2, length(dir) + 1)
        }'
}

# Candidate name sets, tried until one comes back from the host out of
# name order. `ls -f` reports directory entries unsorted, in readdir
# order.
SETS="delta Foxtrot charlie Alpha echobox bravo
Zulu yankee Xray whiskey Victor uniform
mike November oscar Papa quebec Romeo
Golf hotel India juliet Kilo lima"

NAMES=""
SCRAMBLED=no
for setno in 1 2 3 4; do
    line="$(printf '%s\n' "$SETS" | sed -n "${setno}p")"
    rm -rf "${WORK:?}/bin"
    mkdir "$WORK/bin"
    for name in $line; do
        printf 'contents of %s\n' "$name" > "$WORK/bin/$name"
        chmod 755 "$WORK/bin/$name"
    done
    NAMES="$line"
    # `find` reports entries in readdir order, which is the whole question
    # here: a shell glob would sort them and answer it by construction.
    HOST_ORDER="$(find "$WORK/bin" -type f -exec basename {} \;)"
    SORTED_ORDER="$(printf '%s\n' "$HOST_ORDER" | sort)"
    if [ "$HOST_ORDER" != "$SORTED_ORDER" ]; then
        SCRAMBLED=yes
        break
    fi
done

{
    printf 'dir 0755 /bin\n'
    printf 'exec-dir 0755 /bin %s\n' "$WORK/bin"
} > "$WORK/m.manifest"

stage "$WORK/m.manifest" "$WORK/one.img"
stage "$WORK/m.manifest" "$WORK/two.img"

# The clock half. mkfs used to stamp the root with the host's time of day
# at microsecond resolution, so two stagings are never in the same instant.
cmp -s "$WORK/one.img" "$WORK/two.img" || \
    fail "one manifest staged twice produced two different images" \
         "the root node's timestamps are the usual reason"

# The order half. The expectation is the EXACT list, not merely a sorted
# one: a stager that dropped an entry leaves a list that is still in
# order, and one that folded case leaves one in a different order than
# this comparator's.
IMAGE_ORDER="$(staged_names "$WORK/one.img" /bin)"
WANT_ORDER="$(for n in $NAMES; do printf '%s\n' "$n"; done | sort)"
[ "$IMAGE_ORDER" = "$WANT_ORDER" ] || \
    fail "the staged names are $(printf '%s' "$IMAGE_ORDER" | tr '\n' ' '), and name order is $(printf '%s' "$WANT_ORDER" | tr '\n' ' ')" \
         "exec-dir must sort its entries the way tree does"

# The tree half: a nested source tree, staged twice through the recursive
# directive, must produce identical bytes AND hold its entries in sorted
# order at every level, asserted against the exact expected walk, so an
# unsorted walk cannot pass by reading one host's directories twice in the
# same accidental order. This is the shape /src/rach2 takes in os-real.
mkdir -p "$WORK/srctree/sub"
for name in $NAMES; do
    printf 'contents of %s\n' "$name" > "$WORK/srctree/$name"
    printf 'contents of %s\n' "$name" > "$WORK/srctree/sub/$name"
done
printf 'all:\n' > "$WORK/srctree/guest.mk"
{
    printf 'dir 0755 /src\n'
    printf 'tree 0644 /src/demo %s\n' "$WORK/srctree"
} > "$WORK/t.manifest"
stage "$WORK/t.manifest" "$WORK/tree-one.img"
stage "$WORK/t.manifest" "$WORK/tree-two.img"
cmp -s "$WORK/tree-one.img" "$WORK/tree-two.img" || \
    fail "one source tree staged twice produced two different images"

# Every staged node, the root included, must carry the one fixed instant
# {1, 0}: constant keeps the image reproducible, and nonzero is what lets
# a make run over the staged sources in place, since pdpmake reads a zero
# modification time as a file that does not exist. A revert to zero would
# pass the byte comparison above, so the value itself is asserted here.
BADTIME="$("$SETID" --nodes --image "$WORK/tree-one.img" | awk '
    $3 != 1 || $4 != 0 || $5 != 1 || $6 != 0 || $7 != 1 || $8 != 0 {
        print $3, $4; exit
    }')"
[ -z "$BADTIME" ] || \
    fail "a staged node's timestamp is ($(echo "$BADTIME" | tr ' ' ',')), not the fixed staged instant (1, 0)"

# The expected walk: the top level sorted, each subdirectory's entries in
# place behind it.
WALK="$( { for n in $NAMES; do printf '%s\n' "$n"; done
           printf 'guest.mk\nsub\n'; } | sort |
         while read -r entry; do
             printf '%s\n' "$entry"
             if [ "$entry" = sub ]; then
                 for n in $(for x in $NAMES; do printf '%s\n' "$x"; done | sort); do
                     printf 'sub/%s\n' "$n"
                 done
             fi
         done)"
TREE_ORDER="$(staged_names "$WORK/tree-one.img" /src/demo)"
[ "$TREE_ORDER" = "$WALK" ] || \
    fail "the staged tree order is $(printf '%s' "$TREE_ORDER" | tr '\n' ' '), and the sorted walk is $(printf '%s' "$WALK" | tr '\n' ' ')" \
         "tree must sort every directory it walks"

if [ "$SCRAMBLED" = no ]; then
    # Every set this check knows came back already in name order on this
    # host, so the assertion above held without the stager having to sort
    # anything. Said out loud, not reported as a pass.
    echo "check-repro: OK (one tree, one image; the clock and the tree directive excluded). This host reports directory entries in name order, so the ordering half was not exercised"
    finished=1
    exit 0
fi
echo "check-repro: OK (one tree, one image; the clock and the host's directory order both excluded)"
finished=1
