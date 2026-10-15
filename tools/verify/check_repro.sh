#!/usr/bin/env sh

#
# check_repro.sh: check that one tree stages to one image, whatever host
# builds it.
#
# A staged image is built from a manifest and a set of host artifacts, and
# nothing about the building host may show in it. Two things could: the
# host's clock, if mkfs or the stager stamped a node with the time of day,
# and the host file system's directory order (a hash of the names on one
# system, creation order on another), if `exec-dir` or `tree` staged entries
# in the order `readdir` returns them.
#
# Staging one directory twice cannot find an order fault, since one
# directory returns one order both times. The check asserts the property the
# stager's sort gives instead: the names are in the image in name order. That
# is a test only where the host's own order differs from name order, so the
# check tries name sets until one does, and says so when none does. Every set
# mixes case: under strcmp every capital sorts before every lower-case
# letter, so a stager that sorted with a case-folding comparator fails too.
#
# The node inventory comes from `setidcheck --nodes`, which prints each root
# or reserved node in ring order with its path and timestamps.
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
make -s -C "$REPO" build/tools/inspect/setidcheck || exit 1

WORK="$(mktemp -d "${TMPDIR:-/tmp}/rach2_repro.XXXXXX")"
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf "$WORK"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'rm -rf "$WORK"' INT TERM

# fail WHAT [WHY]: report a failure, with its likely cause, and exit 1.
fail() {
    echo "check-repro: FAIL: $1"
    [ -z "${2:-}" ] || echo "check-repro: $2"
    exit 1
}

# stage MANIFEST IMAGE: make a 1 MiB KRFFS image and stage MANIFEST into it.
stage() {
    dd if=/dev/zero of="$2" bs=1048576 count=1 2>/dev/null
    "$MKFS" "$2" >/dev/null || fail "$MKFS failed"
    "$STAGER" "$1" "$2" >/dev/null || fail "$STAGER failed"
}

# staged_names IMAGE DIR: print the names under the guest directory DIR, in
# the order the image has them.
staged_names() {
    "$SETID" --nodes --image "$1" | awk -v dir="$2/" '
        $1 == "reserved" && index($2, dir) == 1 {
            print substr($2, length(dir) + 1)
        }'
}

# Candidate name sets, one a line, tried until the host returns one out of
# name order.
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
    # find reports entries in readdir order; a shell glob would sort them.
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

# The clock: a node stamped with the host's time of day makes two stagings
# differ.
cmp -s "$WORK/one.img" "$WORK/two.img" || \
    fail "one manifest staged twice produced two different images" \
         "the root node's timestamps are the usual reason"

# The order of exec-dir. The image's list must equal the sorted list of the
# names: a stager that dropped an entry would leave a list still in order,
# and one that folded case would leave one in a different order from sort's.
IMAGE_ORDER="$(staged_names "$WORK/one.img" /bin)"
WANT_ORDER="$(for n in $NAMES; do printf '%s\n' "$n"; done | sort)"
[ "$IMAGE_ORDER" = "$WANT_ORDER" ] || \
    fail "the staged names are $(printf '%s' "$IMAGE_ORDER" | tr '\n' ' '), and name order is $(printf '%s' "$WANT_ORDER" | tr '\n' ' ')" \
         "exec-dir must sort its entries the way tree does"

# The order of tree: a nested source tree, staged twice through the recursive
# directive, must give identical bytes and have its entries in sorted order
# at every level. The order is compared with the expected walk, since an
# unsorted walk would read one host's directories in the same order twice.
# os-real stages /src/rach2 this way.
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

# Every staged node, the root included, must have the fixed instant {1, 0}
# in each of its three timestamps. A constant keeps the image reproducible,
# and a nonzero one lets make run over the staged sources in place, since
# pdpmake reads a zero modification time as a file that does not exist. A
# zero instant would pass the byte comparison above, so the value is
# compared here.
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
    # Every set came back in name order, so the stager's sort is untested.
    echo "check-repro: OK (one tree, one image; the clock and the tree directive excluded). This host reports directory entries in name order, so the ordering half was not exercised"
    finished=1
    exit 0
fi
echo "check-repro: OK (one tree, one image; the clock and the host's directory order both excluded)"
finished=1
