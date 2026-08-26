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
# Every run also checks the manifest's `embed-cfg` record against the
# current tools/image/grub-embed.cfg. The disk cores embed that file as it
# was when they were generated; an edit to it without a regeneration would
# otherwise produce disks whose embedded configuration nobody checked.
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

# The embedded configuration, checked on every run: the pinned cores carry
# the configuration recorded here, so an edited source file means the cores
# no longer say what the tree says and must be regenerated.
EMBED="$REPO/tools/image/grub-embed.cfg"
want_embed="$(awk '/^embed-cfg /{print $2}' "$MANIFEST")"
got_embed="$("$SHA" "$EMBED" | awk '{print $1}')"
if [ "$got_embed" != "$want_embed" ]; then
    echo "verify_grub: tools/image/grub-embed.cfg does not match the manifest" >&2
    echo "  sha256 $got_embed" >&2
    echo "  manifest $want_embed" >&2
    echo "  the pinned cores embed the recorded configuration; run" >&2
    echo "  vendor/grub/regen_grub.sh after changing it" >&2
    status=1
fi

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
