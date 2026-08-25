#!/usr/bin/env sh

#
# verify_grub.sh: check pinned GRUB artifacts against manifest.txt.
#
# Every consumer of a pinned boot artifact runs this first, the way the
# vendor tarball builds verify their archives: a byte of boot code nobody
# chose must not reach a disk. With no argument every artifact in the
# manifest is checked; with artifact names only those are.
#
# Usage: ./verify_grub.sh [ARTIFACT ...]
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="$SCRIPT_DIR/manifest.txt"

[ -f "$MANIFEST" ] || {
    echo "verify_grub: $MANIFEST is missing" >&2; exit 1; }

SHA="$REPO/build/tools/verify/sha256"
if [ ! -x "$SHA" ]; then
    mkdir -p "${SHA%/*}"
    cc -O2 -o "$SHA" "$REPO/tools/verify/sha256.c"
fi

check_one() {
    name="$1"
    line="$(grep "^artifact $name " "$MANIFEST" || true)"
    [ -n "$line" ] || {
        echo "verify_grub: $name is not in the manifest" >&2; return 1; }
    want_size="$(printf '%s' "$line" | awk '{print $3}')"
    want_hash="$(printf '%s' "$line" | awk '{print $4}')"
    [ -f "$SCRIPT_DIR/$name" ] || {
        echo "verify_grub: $name is missing beside the manifest" >&2; return 1; }
    got_size="$(wc -c < "$SCRIPT_DIR/$name" | tr -d ' ')"
    got_hash="$("$SHA" "$SCRIPT_DIR/$name" | awk '{print $1}')"
    if [ "$got_size" != "$want_size" ] || [ "$got_hash" != "$want_hash" ]; then
        echo "verify_grub: $name does not match the manifest" >&2
        echo "  size $got_size (manifest $want_size)" >&2
        echo "  sha256 $got_hash" >&2
        echo "  manifest $want_hash" >&2
        return 1
    fi
    return 0
}

status=0
if [ $# -gt 0 ]; then
    for name in "$@"; do
        check_one "$name" || status=1
    done
else
    # shellcheck disable=SC2013 # artifact names are single tokens by format
    for name in $(awk '/^artifact /{print $2}' "$MANIFEST"); do
        check_one "$name" || status=1
    done
fi
exit $status
