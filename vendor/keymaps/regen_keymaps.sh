#!/usr/bin/env sh

#
# regen_keymaps.sh: regenerate the keymaps in xkb/ from XKB's data. Maintainer
# only.
#
# The build stages the committed maps and never runs ckbcomp; this procedure
# is how the maps are made when the layouts, the add-ons or the data change
# on purpose. As vendor/grub/regen_grub.sh does, it runs inside the pinned
# Ubuntu container with apt pointed at one dated state of the Ubuntu snapshot
# archive, makes everything twice from clean directories, requires the two
# rounds byte-identical, and writes manifest.txt with the container digest,
# the snapshot, each package's version and .deb digest, every package the
# install added, the layouts, and each output's size and digest.
# gen_keymaps.sh does the work in the container, and check_keymap.awk checks
# every map and add-on there before anything is staged.
#
# Usage: sh vendor/keymaps/regen_keymaps.sh
#
# SNAPSHOT below pins the archive state; a maintainer moving to newer data
# raises it and commits the result.
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

UBUNTU_IMAGE="$(sed -n 's/^\(ubuntu@sha256:[0-9a-f]*\)$/\1/p' \
                "$REPO/tools/image/ubuntu-image.txt" | head -1)"
[ -n "$UBUNTU_IMAGE" ] || {
    echo "regen_keymaps: tools/image/ubuntu-image.txt has no digest" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || {
    echo "regen_keymaps: docker is required for this maintainer procedure" >&2
    exit 1
}

SNAPSHOT="${RACH2_KEYMAPS_SNAPSHOT:-20260825T013635Z}"
SNAPURL="https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT"
SHA="$REPO/build/tools/verify/sha256"
make -s -C "$REPO" build/tools/verify/sha256

# One generation round into $1.
round() {
    out="$1"
    mkdir -p "$out"
    docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" \
        "$UBUNTU_IMAGE" sh "$SCRIPT_DIR/gen_keymaps.sh" "$out" "$SNAPURL"
    # The digests are taken on the host with the project tool, as
    # regen_grub.sh takes them.
    {
        sed -n '/^#/p' "$out/manifest.head"
        echo "container $UBUNTU_IMAGE"
        sed '/^#/d' "$out/manifest.head"
        for f in $(cd "$out" && ls -- *.map) common.inc; do
            printf 'map %s %s %s\n' "$f" "$(wc -c < "$out/$f" | tr -d ' ')" \
                "$("$SHA" "$out/$f" | awk '{print $1}')"
        done
    } > "$out/manifest.txt"
    rm -f "$out/manifest.head"
}

R1="$REPO/build/keymaps-regen/round1"
R2="$REPO/build/keymaps-regen/round2"
rm -rf "$REPO/build/keymaps-regen"
echo "regen_keymaps: round 1"
round "$R1"
echo "regen_keymaps: round 2"
round "$R2"

for f in $(cd "$R1" && ls -- *.map) common.inc manifest.txt; do
    cmp -s "$R1/$f" "$R2/$f" || {
        echo "regen_keymaps: $f differs between the two rounds; not reproducible" >&2
        exit 1
    }
done

echo "regen_keymaps: the two rounds agree; staging into vendor/keymaps/"
rm -f "$SCRIPT_DIR"/xkb/*.map
cp "$R1"/*.map "$R1/common.inc" "$SCRIPT_DIR/xkb/"
cp "$R1/manifest.txt" "$SCRIPT_DIR/manifest.txt"
rm -rf "$REPO/build/keymaps-regen"
set -- "$SCRIPT_DIR"/xkb/*.map
echo "regen_keymaps: result: staged $# maps"
