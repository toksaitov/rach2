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
# any previous image intact rather than a half-written one that run.sh would boot.
TMP="$OUT.new"
trap 'rm -f "$TMP"' EXIT INT TERM
dd if=/dev/zero of="$TMP" bs=1048576 count="$SIZE" 2>/dev/null
"$TOOLS/mkfs.krffs" "$TMP" >/dev/null
"$TOOLS/stageimage.krffs" "$MANIFEST" "$TMP"
mv "$TMP" "$OUT"
