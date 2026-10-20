#!/bin/sh
# Check the KRFFS host tools: the stager's refusals, the image builder's
# replace, and what the visualiser, the defragmenter and the node editor
# print or change, the defragmenter on a whole image and on a region of a
# larger file.
#
# Each refusal of the stager is driven by a manifest of its own and must end
# with a non-zero status and the stager's words for it: a malformed path, a
# path under /dev or /proc, a path staged twice into an image or into the
# tree the ext2 builder reads, a directory under a file, a file that does not
# fit the image, and a second directory whose name mixes to an id another
# directory already has, named in a `dir` line or made for a file's path.
# Every row makes its own small image with mkfs.krffs.
#
# `--build TREE OUT` compiles the six tools from TREE/krffs, with the flags
# krffs/Makefile gives them, into OUT/bin, and lays out OUT/tree, where the
# image builder finds them under its own ../build/krffs. `OUT` alone runs
# every row on the tools there; tests/proofs/krffs-tools.json builds them
# from private copies of the sources and proves the rows against faults in
# them, and the host suite runs its base phase.
#
# Usage: sh tests/host/krffs_tools_test.sh --build TREE OUT
#        sh tests/host/krffs_tools_test.sh OUT
set -eu

case $# in
    3) [ "$1" = --build ] || { echo 'usage: krffs_tools_test.sh [--build TREE] OUT' >&2; exit 2; } ;;
    1) ;;
    *) echo 'usage: krffs_tools_test.sh [--build TREE] OUT' >&2; exit 2 ;;
esac
if [ "$1" = --build ]; then
    out=$(mkdir -p "$3" && CDPATH='' cd -- "$3" && pwd)
    cd "$2"
    rm -rf "${out:?}/bin" "${out:?}/tree"
    mkdir -p "$out/bin" "$out/tree/krffs" "$out/tree/build/krffs"
    # The flags krffs/Makefile gives the tools, fsviz's exception among them.
    flags='-O2 -Wall -Wextra -Werror -std=gnu99 -D_FILE_OFFSET_BITS=64 -I krffs'
    core='krffs/krffs_file_system.c krffs/krffs_node.c krffs/krffs_utilities.c'
    core="$core krffs/krffs_platform.c"
    # shellcheck disable=SC2086 # the flags and the core are lists of words
    {
        "${CC:-cc}" $flags -o "$out/bin/mkfs.krffs" $core krffs/mkfs.krffs.c
        "${CC:-cc}" $flags -o "$out/bin/fsck.krffs" $core krffs/krffs_allocator.c \
            krffs/fsck.krffs.c
        "${CC:-cc}" $flags -o "$out/bin/stageimage.krffs" $core krffs/krffs_allocator.c \
            krffs/stageimage.krffs.c
        "${CC:-cc}" $flags -Wno-error=unused-result -o "$out/bin/fsviz.krffs" $core \
            krffs/fsviz.krffs.c
        "${CC:-cc}" $flags -o "$out/bin/defrag.krffs" $core krffs/defrag.krffs.c
        "${CC:-cc}" $flags -o "$out/bin/edfs.krffs" $core krffs/edfs.krffs.c
    }
    cp krffs/mkimage.sh "$out/tree/krffs/mkimage.sh"
    cp "$out/bin/mkfs.krffs" "$out/bin/stageimage.krffs" "$out/tree/build/krffs/"
    exit 0
fi
tools=$(CDPATH='' cd -- "$1" && pwd)
child=
work=
# cleanup: stop the running command and remove the scratch directory.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-krffs-tools.XXXXXX")
dir=$work

# Run one command with a time limit: its output in the file named first, its
# status in $rc.
bounded()
{
    out=$1
    shift
    perl -e 'alarm 10; exec @ARGV or die "krffs-tools: exec: $!\n"' "$@" >"$out" 2>&1 &
    child=$!
    rc=0
    wait "$child" || rc=$?
    child=
}

# pass ROW: report ROW passed.
pass()
{
    echo "krffs-tools: result: pass: $1"
}

# fail ROW DETAIL: report ROW failed with DETAIL, and count it in $failed.
fail()
{
    echo "krffs-tools: result: fail: $1: $2"
    failed=$((failed + 1))
}

# A fresh image of $3 KiB with the manifest $2 staged into it; the stager's
# status is in $rc.
staged()
{
    dd if=/dev/zero of="$1" bs=1024 count="$3" 2>/dev/null
    if "$tools/bin/mkfs.krffs" "$1" >/dev/null 2>&1; then
        bounded "$1.stage" "$tools/bin/stageimage.krffs" "$2" "$1"
    else
        rc=1
        echo "mkfs.krffs failed" >"$1.stage"
    fi
}

# A manifest the stager must refuse, and the words of the refusal, which name
# what it refused.
refusal()
{
    printf '%b' "$2" >"$dir/$1.manifest"
    staged "$dir/$1.krffs" "$dir/$1.manifest" 64
    if [ "$rc" -ne 0 ] && grep -qF "$3" "$dir/$1.krffs.stage"; then
        pass "$1"
    else
        fail "$1" "status $rc: $(cat "$dir/$1.krffs.stage")"
    fi
}

# The same refusal where the stager writes the manifest into a directory for
# the ext2 builder, `stageimage MANIFEST --tree DIR`, instead of an image.
tree_refusal()
{
    printf '%b' "$2" >"$dir/$1.manifest"
    bounded "$dir/$1.stage" "$tools/bin/stageimage.krffs" "$dir/$1.manifest" --tree \
        "$dir/$1.tree"
    if [ "$rc" -ne 0 ] && grep -qF "$3" "$dir/$1.stage"; then
        pass "$1"
    else
        fail "$1" "status $rc: $(cat "$dir/$1.stage")"
    fi
}

# A file larger than the whole image: the allocator finds no node for it, and
# the stager must say so and fail rather than stage what fits.
full_row()
{
    dd if=/dev/zero of="$dir/big.bin" bs=1024 count=128 2>/dev/null
    refusal stager-full "file 0644 /big $dir/big.bin\n" "not enough space for 'big'"
}

# A manifest that fails part way leaves the previous image as it was: the
# builder stages into a temporary and renames it into place only on success.
replace_row()
{
    printf 'text 0644 /old.txt previous\n' >"$dir/old.manifest"
    mkdir "$dir/out"
    staged "$dir/out/image.krffs" "$dir/old.manifest" 1024
    rm -f "$dir/out/image.krffs.stage"
    cp "$dir/out/image.krffs" "$dir/before.krffs"
    printf 'text 0644 /new.txt next\nfile 0644 /missing.bin %s\n' "$dir/absent" \
        >"$dir/bad.manifest"
    bounded "$dir/replace.log" sh "$tools/tree/krffs/mkimage.sh" "$dir/bad.manifest" \
        "$dir/out/image.krffs" 1
    if [ "$rc" -ne 0 ] && cmp -s "$dir/before.krffs" "$dir/out/image.krffs" &&
        [ "$(ls "$dir/out")" = image.krffs ]; then
        pass replace-keeps-image
    else
        fail replace-keeps-image "status $rc; the image changed or a temporary remained"
    fi
}

# The visualiser draws the ring in order: the root, one bar for each staged
# node under its name, and the free space.
fsviz_row()
{
    staged "$dir/fsviz.krffs" "$dir/three.manifest" 64
    bounded "$dir/fsviz.out" "$tools/bin/fsviz.krffs" "$dir/fsviz.krffs" -t
    bars=$(grep -o '\[[*#-] *[a-z ]*' "$dir/fsviz.out" | sed 's/  */ /g' | tr '\n' '|')
    if [ "$rc" -eq 0 ] && [ "$bars" = '[* root node|[# alpha|[# beta|[# gamma|[- free node|' ]; then
        pass fsviz-ribbon
    else
        fail fsviz-ribbon "status $rc, bars $bars"
    fi
}

# The defragmenter closes a hole: beta's node is marked free, and afterwards
# the reserved nodes follow the root in their order with one free node last.
# The defragmenter runs fsck.krffs first and stops if it fails, so the check
# of the holed image is part of the row. Its status is 1 when a moved node's
# sync to the file fails, as msync does from an address inside a page.
defrag_row()
{
    staged "$dir/defrag.krffs" "$dir/three.manifest" 64
    at=$(python3 "$work/ring.py" at "$dir/defrag.krffs" beta)
    bounded "$dir/hole.log" "$tools/bin/edfs.krffs" "$dir/defrag.krffs" "$at" --set-type free
    before=$(python3 "$work/ring.py" names "$dir/defrag.krffs")
    bounded "$dir/defrag.log" env PATH="$tools/bin:$PATH" "$tools/bin/defrag.krffs" \
        "$dir/defrag.krffs"
    after=$(python3 "$work/ring.py" names "$dir/defrag.krffs")
    if [ "$before" = 'root alpha free gamma free' ] && [ "$rc" -eq 0 ] &&
        [ "$after" = 'root alpha gamma free' ]; then
        pass defrag-compacts
    else
        fail defrag-compacts "status $rc, before '$before', after '$after'"
    fi
}

# The same hole closed in a region of a larger file, 64 KiB at 1 MiB with
# zeros before and after it, given to every tool as --offset and --size. The
# check that the defragmenter runs first must read that region too, since the
# zeros at the start of the file are no file system.
defrag_region_row()
{
    region='--offset 1048576 --size 65536'
    dd if=/dev/zero of="$dir/region.img" bs=1024 count=1152 2>/dev/null
    # shellcheck disable=SC2086
    if "$tools/bin/mkfs.krffs" "$dir/region.img" $region >/dev/null 2>&1; then
        bounded "$dir/region.stage" "$tools/bin/stageimage.krffs" "$dir/three.manifest" \
            "$dir/region.img" $region
    else
        rc=1
    fi
    dd if="$dir/region.img" of="$dir/region.part" bs=65536 skip=16 count=1 2>/dev/null
    at=$(python3 "$work/ring.py" at "$dir/region.part" beta)
    # shellcheck disable=SC2086
    bounded "$dir/region-hole.log" "$tools/bin/edfs.krffs" "$dir/region.img" "$at" \
        --set-type free $region
    dd if="$dir/region.img" of="$dir/region.part" bs=65536 skip=16 count=1 2>/dev/null
    before=$(python3 "$work/ring.py" names "$dir/region.part")
    # shellcheck disable=SC2086
    bounded "$dir/region-defrag.log" env PATH="$tools/bin:$PATH" "$tools/bin/defrag.krffs" \
        "$dir/region.img" $region
    dd if="$dir/region.img" of="$dir/region.part" bs=65536 skip=16 count=1 2>/dev/null
    after=$(python3 "$work/ring.py" names "$dir/region.part")
    if [ "$before" = 'root alpha free gamma free' ] && [ "$rc" -eq 0 ] &&
        [ "$after" = 'root alpha gamma free' ]; then
        pass defrag-region
    else
        fail defrag-region "status $rc, before '$before', after '$after'"
    fi
}

# The node editor writes the field it is asked for and nothing else: the
# root's magic becomes 0x1234, two bytes, little-endian.
edfs_row()
{
    staged "$dir/edfs.krffs" "$dir/three.manifest" 64
    cp "$dir/edfs.krffs" "$dir/edfs.before"
    bounded "$dir/edfs.log" "$tools/bin/edfs.krffs" "$dir/edfs.krffs" 0 --set-magic 4660
    changed=$(python3 "$work/ring.py" changed "$dir/edfs.before" "$dir/edfs.krffs")
    if [ "$rc" -eq 0 ] && [ "$changed" = '0:34 1:12' ]; then
        pass edfs-magic
    else
        fail edfs-magic "status $rc, changed bytes '$changed'"
    fi
}

# A reader of the node ring that shares no code with the tools it judges. The
# field offsets are krffs/krffs_node.h's: the type at 2, the name at 11 and the
# size at 325. `names` prints the ring in order, `at` one node's position and
# `changed` the offsets where the second file differs, with its byte there.
cat >"$work/ring.py" <<'PY'
import struct
import sys


def ring(path):
    data = open(path, "rb").read()
    nodes, position = [], 0
    for _ in range(64):
        if position + 333 > len(data):
            return nodes + [(position, "broken")]
        kind = data[position + 2]
        size = struct.unpack_from("<Q", data, position + 325)[0]
        name = data[position + 11:position + 267].split(b"\0")[0].decode(errors="replace")
        nodes.append((position, ("root", "free", name)[kind] if kind < 3 else "unknown"))
        if size == 0 or position + size > len(data):
            return nodes + [(position, "broken")]
        position += size
        if position == len(data):
            break
    return nodes


if sys.argv[1] == "names":
    print(" ".join(name for _, name in ring(sys.argv[2])))
elif sys.argv[1] == "at":
    print(next((str(p) for p, name in ring(sys.argv[2]) if name == sys.argv[3]), ""))
elif sys.argv[1] == "changed":
    a, b = (open(p, "rb").read() for p in sys.argv[2:4])
    print(" ".join("%d:%02x" % (i, b[i]) for i in range(min(len(a), len(b))) if a[i] != b[i]))
PY

failed=0
refusal stager-doubled-slash 'dir 0755 //etc\n' "malformed directory path '//etc'"
refusal stager-synthetic-dir 'dir 0755 /proc/x\n' \
    "'/proc/x' is under a kernel-synthetic namespace"
refusal stager-synthetic 'text 0644 /dev/console x\n' \
    "'/dev/console' is under a kernel-synthetic namespace"
refusal stager-twice 'text 0644 /a x\ntext 0644 /a y\n' "'/a' is staged more than once"
tree_refusal stager-tree-twice 'text 0644 /a x\ntext 0644 /a y\n' \
    "'/a' is staged more than once"
refusal stager-through-file 'text 0644 /a x\ndir 0755 /a/b\n' \
    "path '/a' descends through non-directory 'a'"
full_row
refusal stager-dir-id 'dir 0755 /bC\ndir 0755 /cb\n' \
    "directory '/cb' has the id of directory '/bC'"
refusal stager-dir-id-implicit 'dir 0755 /bC\ntext 0644 /cb/x y\n' \
    "directory '/cb' has the id of directory '/bC'"
replace_row
printf 'text 0644 /alpha a\ntext 0644 /beta bb\ntext 0644 /gamma ccc\n' >"$dir/three.manifest"
fsviz_row
defrag_row
defrag_region_row
edfs_row
if [ "$failed" -ne 0 ]; then
    echo "krffs-tools: result: fail: $failed rows failed"
    exit 1
fi
echo 'krffs-tools: result: pass: every row'
