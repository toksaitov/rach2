#!/bin/sh
# Prove the KRFFS host tools on private builds: the stager's refusals, the
# image builder's replace, and what the visualiser, the defragmenter and the
# node editor print or change.
#
# Each refusal of the stager is driven by a manifest of its own and must end
# with a non-zero status and the stager's words for it: a malformed path, a
# path under /dev or /proc, a path staged twice into an image or into the
# tree the ext2 builder reads, a directory under a file, a file that does not
# fit the image, and a second directory whose name mixes to an id another
# directory already has, named in a `dir` line or made for a file's path.
# Every row makes its own small image with mkfs.krffs.
#
# With --check the rows run once, over the tools as they are, which is how
# the host suite runs them. With no argument each fault below is put into a
# private copy of one tool, must fail the row named for it, and is restored,
# touched and rebuilt before every row runs again. The tree's own sources are
# never changed.
#
# --texts checks that each fault's text is once in the tree's source, and
# builds and runs nothing.
#
# Usage: sh tests/host/krffs_tools_mutate.sh [--check | --texts]
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || [ "$*" = --texts ] || {
    echo 'krffs-tools-mutation: usage: krffs_tools_mutate.sh [--check | --texts]' >&2; exit 2;
}
texts=
[ "$*" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" krffs-tools-mutation "$repo/tests/host/krffs_tools_mutate.sh" "$@"
fi
child=
sources='stageimage.krffs.c fsviz.krffs.c defrag.krffs.c edfs.krffs.c mkimage.sh'
if [ -n "$texts" ]; then
    src=krffs
else
    mkdir -p build/tests
    work=$(mktemp -d "$repo/build/tests/krffs-tools-proof.XXXXXX")
    src=$work/src
    mkdir -p "$work/src" "$work/bin" "$work/tree/krffs" "$work/tree/build/krffs"
    for f in $sources; do
        cp "krffs/$f" "$work/$f.saved"
    done
fi

# restore: copy the saved sources back over the private ones.
restore()
{
    for f in $sources; do
        cp "$work/$f.saved" "$work/src/$f"
        touch "$work/src/$f"
    done
}

# cleanup: the exit trap; stop the running command and restore the sources.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
if [ -z "$texts" ]; then
    restore
    trap cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
fi

# The flags krffs/Makefile gives the tools, fsviz's exception among them, and
# its directory on the include path, so a private copy of a tool's main
# compiles against the tree's core.
flags='-O2 -Wall -Wextra -Werror -std=gnu99 -D_FILE_OFFSET_BITS=64 -I krffs'
core='krffs/krffs_file_system.c krffs/krffs_node.c krffs/krffs_platform.c krffs/krffs_utilities.c'

# The six tools from the private sources, and the image builder's private tree:
# the script finds its tools under its own ../build/krffs.
build()
{
    log="$work/$1.compile.log"
    # shellcheck disable=SC2086
    if ! { "${CC:-cc}" $flags -o "$work/bin/mkfs.krffs" $core krffs/mkfs.krffs.c &&
        "${CC:-cc}" $flags -o "$work/bin/fsck.krffs" $core krffs/krffs_allocator.c \
            krffs/fsck.krffs.c &&
        "${CC:-cc}" $flags -o "$work/bin/stageimage.krffs" $core krffs/krffs_allocator.c \
            "$work/src/stageimage.krffs.c" &&
        "${CC:-cc}" $flags -Wno-error=unused-result -o "$work/bin/fsviz.krffs" $core \
            "$work/src/fsviz.krffs.c" &&
        "${CC:-cc}" $flags -o "$work/bin/defrag.krffs" $core "$work/src/defrag.krffs.c" &&
        "${CC:-cc}" $flags -o "$work/bin/edfs.krffs" $core "$work/src/edfs.krffs.c"; } \
        >"$log" 2>&1; then
        cat "$log" >&2
        exit 1
    fi
    cp "$work/src/mkimage.sh" "$work/tree/krffs/mkimage.sh"
    cp "$work/bin/mkfs.krffs" "$work/bin/stageimage.krffs" "$work/tree/build/krffs/"
    cksum "$work/src/"* "$work/bin/"* >"$work/$1.cksum"
}

# Run one command with a time limit: its output in the file named first, its
# status in $rc.
bounded()
{
    out=$1
    shift
    perl -e 'alarm 10; exec @ARGV or die "krffs-tools-mutation: exec: $!\n"' "$@" >"$out" 2>&1 &
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
    if "$work/bin/mkfs.krffs" "$1" >/dev/null 2>&1; then
        bounded "$1.stage" "$work/bin/stageimage.krffs" "$2" "$1"
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
    bounded "$dir/$1.stage" "$work/bin/stageimage.krffs" "$dir/$1.manifest" --tree \
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
    bounded "$dir/replace.log" sh "$work/tree/krffs/mkimage.sh" "$dir/bad.manifest" \
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
    bounded "$dir/fsviz.out" "$work/bin/fsviz.krffs" "$dir/fsviz.krffs" -t
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
# of the holed image is part of the row.
defrag_row()
{
    staged "$dir/defrag.krffs" "$dir/three.manifest" 64
    at=$(python3 "$work/ring.py" at "$dir/defrag.krffs" beta)
    bounded "$dir/hole.log" "$work/bin/edfs.krffs" "$dir/defrag.krffs" "$at" --set-type free
    before=$(python3 "$work/ring.py" names "$dir/defrag.krffs")
    bounded "$dir/defrag.log" env PATH="$work/bin:$PATH" "$work/bin/defrag.krffs" \
        "$dir/defrag.krffs"
    after=$(python3 "$work/ring.py" names "$dir/defrag.krffs")
    if [ "$before" = 'root alpha free gamma free' ] && [ "$rc" -eq 0 ] &&
        [ "$after" = 'root alpha gamma free' ]; then
        pass defrag-compacts
    else
        fail defrag-compacts "status $rc, before '$before', after '$after'"
    fi
}

# The node editor writes the field it is asked for and nothing else: the
# root's magic becomes 0x1234, two bytes, little-endian.
edfs_row()
{
    staged "$dir/edfs.krffs" "$dir/three.manifest" 64
    cp "$dir/edfs.krffs" "$dir/edfs.before"
    bounded "$dir/edfs.log" "$work/bin/edfs.krffs" "$dir/edfs.krffs" 0 --set-magic 4660
    changed=$(python3 "$work/ring.py" changed "$dir/edfs.before" "$dir/edfs.krffs")
    if [ "$rc" -eq 0 ] && [ "$changed" = '0:34 1:12' ]; then
        pass edfs-magic
    else
        fail edfs-magic "status $rc, changed bytes '$changed'"
    fi
}

# rows PHASE: run every row in a directory $work/PHASE of its own; $failed is
# the count of rows that failed.
rows()
{
    dir="$work/$1"
    mkdir "$dir"
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
    printf 'text 0644 /alpha a\ntext 0644 /beta bb\ntext 0644 /gamma ccc\n' \
        >"$dir/three.manifest"
    fsviz_row
    defrag_row
    edfs_row
}

# One phase: the tools built from the private sources as they stand, and
# every row. The fixed and restored phases must pass every row; a faulty one
# must fail the row named for its fault.
run_phase()
{
    phase=$1
    expected=$2
    build "$phase"
    rows "$phase" >"$work/$phase.log"
    cat "$work/$phase.log"
    if [ -z "$expected" ]; then
        [ "$failed" -eq 0 ] || exit 1
    else
        grep -q "^krffs-tools: result: fail: $expected:" "$work/$phase.log" || exit 1
    fi
    echo "krffs-tools-mutation: result: $phase: $failed rows failed"
}

# Change one private source where the old text occurs exactly once; with
# --texts, only check that the text is once in the tree's source.
mutate()
{
    python3 - "$src/$1" "$2" "$3" <<'PY'
import os
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("krffs-tools-mutation: source pattern is not unique: " + old)
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(old, new))
PY
    [ -n "$texts" ] || touch "$src/$1"
}

# apply_fault FAULT: put FAULT into its private source, and name in `row`
# the row it must fail.
apply_fault()
{
    # The dollars are the image builder's own, left for its shell.
    # shellcheck disable=SC2016
    case $1 in
        doubled-slash)
            mutate stageimage.krffs.c 'strstr(path, "//") != NULL || ' ''
            row=stager-doubled-slash ;;
        synthetic-dir)
            mutate stageimage.krffs.c 'strncmp(path, "/proc/", 6) == 0' '0'
            row=stager-synthetic-dir ;;
        synthetic)
            mutate stageimage.krffs.c 'strncmp(path, "/dev/", 5) == 0' '0'
            row=stager-synthetic ;;
        twice)
            mutate stageimage.krffs.c 'if (find_child(file_system, dir_id, name) != NULL) {' \
                'if (0) {'
            row=stager-twice ;;
        tree-twice)
            mutate stageimage.krffs.c 'if (fd < 0 && errno == EEXIST) {' 'if (0) {'
            row=stager-tree-twice ;;
        through-file)
            mutate stageimage.krffs.c 'if (!is_dir_node(existing)) {' 'if (0) {'
            row=stager-through-file ;;
        full)
            mutate stageimage.krffs.c 'if (!node) {' 'if (!node) { return 1;'
            row=stager-full ;;
        dir-id)
            mutate stageimage.krffs.c \
                'if (!dir_id_free(file_system, parent_id, name, path)) {' 'if (0) {'
            row=stager-dir-id ;;
        dir-id-implicit)
            mutate stageimage.krffs.c 'if (!dir_id_free(file_system, parent, p, here)) {' \
                'if (0) {'
            row=stager-dir-id-implicit ;;
        replace)
            mutate mkimage.sh 'TMP="$OUT.new.$$"' 'TMP="$OUT"'
            row=replace-keeps-image ;;
        fsviz)
            mutate fsviz.krffs.c '"[# %*s: "' '"[- %*s: "'
            row=fsviz-ribbon ;;
        defrag)
            mutate defrag.krffs.c 'if (free_node) {' 'if (0) {'
            row=defrag-compacts ;;
        edfs)
            mutate edfs.krffs.c 'node->magic = magic;' '(void)magic;'
            row=edfs-magic ;;
    esac
}

faults='doubled-slash synthetic-dir synthetic twice tree-twice through-file full'
faults="$faults dir-id dir-id-implicit replace fsviz defrag edfs"
if [ -n "$texts" ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in $faults; do
        apply_fault "$fault"
    done
    echo 'krffs-tools-mutation: texts: every fault is once in its source'
    exit 0
fi

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
echo "krffs-tools-mutation: logs: $work"
run_phase fixed ''
[ "${1:-}" != --check ] || exit 0
for fault in $faults; do
    apply_fault "$fault"
    run_phase "$fault-mutated" "$row"
    restore
    run_phase "$fault-restored" ''
    for f in $sources; do
        cmp "$work/$f.saved" "$work/src/$f"
    done
done
echo 'krffs-tools-mutation: result: pass: thirteen faults failed their rows; restored rows passed'
