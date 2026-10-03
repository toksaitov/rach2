#!/usr/bin/env sh

#
# verify_grub.sh: check pinned GRUB artifacts against manifest.txt.
#
# Every consumer of a pinned boot artifact runs this first, the way the
# vendor tarball builds verify their archives: a byte of boot code nobody
# chose must not reach a disk. With no argument every artifact in the
# manifest is checked beside it; with arguments only those are, and an
# argument may be `NAME=PATH` to check a copy at PATH against NAME's
# manifest entry, so a consumer can verify the very bytes it is about to
# read instead of the originals it copied them from a moment earlier.
#
# Every run also checks the manifest's `embed-cfg` and `efi-embed-cfg`
# records against the current tools/image/grub-embed.cfg and
# tools/image/grub-efi-embed.cfg. The BIOS disk cores and the EFI image embed
# those files as they were when they were generated; an edit to either
# without a regeneration would otherwise produce disks whose embedded
# configuration nobody checked.
#
# Usage: ./verify_grub.sh [ARTIFACT[=PATH] ...]
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="$SCRIPT_DIR/manifest.txt"

[ -f "$MANIFEST" ] || {
    echo "verify_grub: $MANIFEST is missing" >&2; exit 1; }

SHA="$REPO/build/tools/verify/sha256"
if [ ! -x "$SHA" ] || [ "$REPO/tools/verify/sha256.c" -nt "$SHA" ]; then
    mkdir -p "${SHA%/*}"
    cc -O2 -o "$SHA" "$REPO/tools/verify/sha256.c"
fi

check_one() {
    name="${1%%=*}"
    case "$1" in
        *=*) path="${1#*=}" ;;
        *)   path="$SCRIPT_DIR/$name" ;;
    esac
    line="$(grep "^artifact $name " "$MANIFEST" || true)"
    [ -n "$line" ] || {
        echo "verify_grub: $name is not in the manifest" >&2; return 1; }
    want_size="$(printf '%s' "$line" | awk '{print $3}')"
    want_hash="$(printf '%s' "$line" | awk '{print $4}')"
    [ -f "$path" ] || {
        echo "verify_grub: $path is missing" >&2; return 1; }
    got_size="$(wc -c < "$path" | tr -d ' ')"
    got_hash="$("$SHA" "$path" | awk '{print $1}')"
    if [ "$got_size" != "$want_size" ] || [ "$got_hash" != "$want_hash" ]; then
        echo "verify_grub: $path does not match the manifest entry for $name" >&2
        echo "  size $got_size (manifest $want_size)" >&2
        echo "  sha256 $got_hash" >&2
        echo "  manifest $want_hash" >&2
        return 1
    fi
    return 0
}

status=0

# The embedded configurations, checked on every run: the pinned images carry
# the configurations recorded here, so an edited source file means the images
# no longer say what the tree says and must be regenerated.
check_embed() { # check_embed RECORD FILE
    want_embed="$(awk -v r="$1" '$1 == r {print $2}' "$MANIFEST")"
    got_embed="$("$SHA" "$REPO/$2" | awk '{print $1}')"
    if [ "$got_embed" != "$want_embed" ]; then
        echo "verify_grub: $2 does not match the manifest" >&2
        echo "  sha256 $got_embed" >&2
        echo "  manifest $want_embed" >&2
        echo "  the pinned images embed the recorded configuration; run" >&2
        echo "  vendor/grub/regen_grub.sh after changing it" >&2
        status=1
    fi
}
check_embed embed-cfg tools/image/grub-embed.cfg
check_embed efi-embed-cfg tools/image/grub-efi-embed.cfg

if [ $# -gt 0 ]; then
    for arg in "$@"; do
        check_one "$arg" || status=1
    done
else
    # shellcheck disable=SC2013 # artifact names are single tokens by format
    for name in $(awk '/^artifact /{print $2}' "$MANIFEST"); do
        check_one "$name" || status=1
    done
fi
exit $status
