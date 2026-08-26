#!/usr/bin/env sh

#
# mkgrubcore.sh: hand out one of the two pinned marked GRUB core images.
#
# The GRUB replacement suites need a core image that announces itself on the
# serial line (`GRUBCORE=A` or `GRUBCORE=B`) so a swap can be proved by the
# core that actually ran. The two marked cores are pinned artifacts under
# vendor/grub/, generated with the rest by regen_grub.sh; this script only
# copies the requested one and verifies the copy against the manifest
# digest, so no GRUB toolchain and no Docker is needed to run the suites.
#
# Usage: ./mkgrubcore.sh {A|B} OUTPUT
#

set -eu

MARKER="${1:-}"
OUT="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

case "$MARKER" in
    A) SRC="$REPO/vendor/grub/disk-core-a.img"; NAME="disk-core-a.img" ;;
    B) SRC="$REPO/vendor/grub/disk-core-b.img"; NAME="disk-core-b.img" ;;
    *)
        echo "usage: mkgrubcore.sh {A|B} OUTPUT" >&2
        exit 2
        ;;
esac
[ -n "$OUT" ] || { echo "usage: mkgrubcore.sh {A|B} OUTPUT" >&2; exit 2; }

# Copy first, verify the copy: the bytes checked are the bytes handed out.
cp "$SRC" "$OUT.new"
if ! sh "$REPO/vendor/grub/verify_grub.sh" "$NAME=$OUT.new"; then
    rm -f "$OUT.new"
    exit 1
fi
mv "$OUT.new" "$OUT"
echo "mkgrubcore: wrote $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes, marker $MARKER)"
