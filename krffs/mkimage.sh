#!/usr/bin/env sh

#
# mkimage.sh: build one KRFFS RAM-disk image out of tree from a manifest.
#
# Zeroes a file of the requested size, lays down a fresh KRFFS on it with
# mkfs.krffs, then stages the manifest with stageimage.krffs. There is no FUSE
# mount: stageimage writes the whole tree (directories and files) directly as v2
# nodes, so this works the same on macOS and Linux without a FUSE driver.
#
# Usage: ./mkimage.sh <manifest> <output.krffs> [size_in_megabytes]
#
# Run from anywhere; the manifest's `build/...` host paths resolve because this
# cd's to the repo root first. The KRFFS host tools come from build/krffs (built
# by `make -C krffs`).
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS="$REPO/build/krffs"

MANIFEST="$1"
OUT="$2"
SIZE="${3:-10}"

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

# Build into a temp file and rename only on success, so a staging failure leaves
# any previous image intact, not a half-written one that run.sh would
# boot.
TMP="$OUT.new"
DEPS_TMP="$OUT.d.new"
trap 'rm -f "$TMP" "$DEPS_TMP"' EXIT INT TERM
dd if=/dev/zero of="$TMP" bs=1048576 count="$SIZE" 2>/dev/null
"$TOOLS/mkfs.krffs" "$TMP" >/dev/null
STAGEIMAGE_DEPS="$DEPS_TMP" "$TOOLS/stageimage.krffs" "$MANIFEST" "$TMP"

# Both halves of the set-id guard, before the rename, so a failing image is
# never moved into place. The sweep reads the manifest, which is where a
# declaration lives: what stages a path, from where, at what mode and owner, and
# for a credential file written inline, what it says. The inventory reads the
# staged artifact, which is the half the text cannot do: exec-dir privileges a
# set of paths the manifest never names, and a parser that has drifted from the
# stager reads a manifest the stager does not. Each catches what the other
# cannot, so a build that ran only one of them would ship what the other
# refuses.
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"
if [ ! -x "$SETIDCHECK" ] || [ "$REPO/tools/inspect/setidcheck.c" -nt "$SETIDCHECK" ]; then
    mkdir -p "${SETIDCHECK%/*}"
    cc -O2 -o "$SETIDCHECK" "$REPO/tools/inspect/setidcheck.c" || exit 1
fi
"$SETIDCHECK" "$MANIFEST"
"$SETIDCHECK" --image "$TMP" --manifest "$MANIFEST"

mv "$TMP" "$OUT"

# Publish the staged inputs as a make dependency file beside the image, after
# the image itself, so a failed build keeps the previous list. A recorded
# path that exists becomes a prerequisite with an empty rule of its own, so
# a later deletion marks the image out of date instead of stopping make. A
# recorded path that is absent (a skipped optional-file) must not be a
# prerequisite, or its empty rule would restage the image on every run;
# its nearest existing ancestor directory stands in for it, because that
# directory's timestamp moves when the file appears.
if [ -s "$DEPS_TMP" ]; then
    DEPS_LIST="$OUT.d.list"
    # Absolute paths, for a reason beyond neatness: a relative recorded path
    # such as `rach2` names the root Makefile's phony component target, and a
    # phony prerequisite is considered remade on every run, which would
    # restage the image forever.
    sort -u "$DEPS_TMP" | while IFS= read -r p; do
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
    {
        printf '%s:' "$OUT"
        while IFS= read -r p; do printf ' %s' "$p"; done < "$DEPS_LIST"
        printf '\n'
        sed 's/$/:/' "$DEPS_LIST"
    } > "$OUT.d"
    rm -f "$DEPS_LIST"
fi
