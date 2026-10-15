#!/usr/bin/env sh

#
# mkimage.sh: build a KRFFS image that fills its file, the form of a RAM
# root, staged from a manifest.
#
# The image is a file of SIZE_MIB mebibytes, 10 by default, which
# mkfs.krffs formats and stageimage.krffs fills with the manifest's tree. No
# file system is mounted, so the build needs neither root nor FUSE.
#
# Usage: mkimage.sh MANIFEST OUTPUT [SIZE_MIB [MIN_FREE_KIB]]
#
# With MIN_FREE_KIB the image must keep at least that many kibibytes free once
# staged, or the build fails: an image a running system writes to, the
# recovery system's among them, needs the room, and contents that grew past
# it would otherwise be found only when a write fails on the machine.
#
# The script runs from any directory: it changes to the repository root,
# against which the manifest's host paths are read. The KRFFS tools must be
# built in build/krffs/ (`make -C krffs`). Beside OUTPUT it writes OUTPUT.d,
# the make dependencies of the image, and OUTPUT.programs for an image with
# `exec-dir` lines.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS="$REPO/build/krffs"

MANIFEST="$1"
OUT="$2"
SIZE="${3:-10}"
MIN_FREE_KIB="${4:-0}"

if [ -z "$MANIFEST" ] || [ -z "$OUT" ]; then
    echo "usage: mkimage.sh <manifest> <output.krffs> [size_mb]" >&2
    exit 2
fi
if [ ! -f "$TOOLS/mkfs.krffs" ] || [ ! -f "$TOOLS/stageimage.krffs" ]; then
    echo "mkimage: the KRFFS host tools are missing; run 'make -C krffs' first" >&2
    exit 1
fi

cd "$REPO"
mkdir -p "$(dirname "$OUT")"

# The image is built in a temporary file and renamed only on success, so a
# failure leaves the previous image as it was. The temporary names have the
# process ID, since two suites of a parallel run can build one image at
# once; both stage the same manifest, so either result is the image, and the
# rename is atomic.
TMP="$OUT.new.$$"
DEPS_TMP="$OUT.d.new.$$"
trap 'rm -f "$TMP" "$DEPS_TMP" "$OUT.d.list.$$" "$OUT.d.$$" "$OUT.programs.$$"' EXIT INT TERM
dd if=/dev/zero of="$TMP" bs=1048576 count="$SIZE" 2>/dev/null
"$TOOLS/mkfs.krffs" "$TMP" >/dev/null
STAGED=$(STAGEIMAGE_DEPS="$DEPS_TMP" "$TOOLS/stageimage.krffs" "$MANIFEST" "$TMP")
printf '%s\n' "$STAGED"
FREE=$(printf '%s\n' "$STAGED" | sed -n 's/.* bytes used, \([0-9]*\) free).*/\1/p' | tail -1)
if [ "$MIN_FREE_KIB" -gt 0 ] && { [ -z "$FREE" ] || [ "$FREE" -lt $((MIN_FREE_KIB * 1024)) ]; }; then
    echo "mkimage: $OUT: ${FREE:-an unknown number of} bytes free, under the $MIN_FREE_KIB KiB it must keep" >&2
    exit 1
fi

# Both halves of the set-id guard, before the rename, so a failing image is
# never moved into place. The sweep reads the manifest: what stages a path,
# from where, at what mode and owner, and, for a credential file written
# inline, its text. The inventory reads the staged image, so it sees the
# paths an `exec-dir` line stages without naming them, and it finds a
# manifest that the guard's parser reads otherwise than the stager does.
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"
make -s -C "$REPO" build/tools/inspect/setidcheck || exit 1
"$SETIDCHECK" "$MANIFEST"
"$SETIDCHECK" --image "$TMP" --manifest "$MANIFEST"

# The programs staged from each `exec-dir` directory, which the dependency
# file lists in place of the directory (krffs/programs.sh). The image is
# touched after them, so that it is the newer of the two: the move keeps the
# time the stager last wrote it, which is before them, and a make that keeps
# times to the nanosecond would find the image out of date at once.
EXEC_DIRS=$(sed -n 's/^exec-dir	//p' "$DEPS_TMP" | sort -u | while IFS= read -r d; do
    case "$d" in
        (/*) printf '%s\n' "$d" ;;
        (*) printf '%s\n' "$REPO/$d" ;;
    esac
done)
if [ -n "$EXEC_DIRS" ]; then
    # shellcheck disable=SC2086 # the directories, one word each
    sh "$REPO/krffs/programs.sh" snapshot $EXEC_DIRS > "$OUT.programs.$$"
    mv "$OUT.programs.$$" "$OUT.programs"
else
    rm -f "$OUT.programs"
fi
touch "$TMP"

mv "$TMP" "$OUT"

# The staged inputs as a make dependency file beside the image, written after
# the image, so a failed build keeps the previous list. A recorded path that
# exists becomes a prerequisite with an empty rule of its own, so a later
# deletion puts the image out of date instead of stopping make. A recorded
# path that is absent, a skipped optional-file, is not a prerequisite, since
# its empty rule would restage the image on every run; its nearest existing
# directory is listed in its place, since that directory's time changes when
# the file appears.
if [ -s "$DEPS_TMP" ]; then
    DEPS_LIST="$OUT.d.list.$$"
    # The paths are made absolute: a relative recorded path such as `rach2`
    # is the name of the root Makefile's phony component target, which make
    # always remakes, so the image would be staged again on every run.
    grep -v '^exec-dir	' "$DEPS_TMP" | sort -u | while IFS= read -r p; do
        case "$p" in
            /*) ;;
            *) p="$REPO/$p" ;;
        esac
        if [ -e "$p" ]; then
            printf '%s\n' "$p"
        else
            d=$(dirname "$p")
            while [ ! -d "$d" ]; do d=$(dirname "$d"); done
            printf '%s\n' "$d"
        fi
    done | sort -u > "$DEPS_LIST"
    [ -z "$EXEC_DIRS" ] || printf '%s\n' "$REPO/$OUT.programs" >> "$DEPS_LIST"
    {
        printf '%s:' "$OUT"
        while IFS= read -r p; do printf ' %s' "$p"; done < "$DEPS_LIST"
        printf '\n'
        sed 's/$/:/' "$DEPS_LIST"
    } > "$OUT.d.$$"
    mv "$OUT.d.$$" "$OUT.d"
    rm -f "$DEPS_LIST"
fi
