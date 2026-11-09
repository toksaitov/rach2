#!/bin/sh
# Stage GE-Proton's verified release for x86-64; i686 has no Proton layer.

set -e
GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
ROOT="$GNU_OUT/proton"
mkdir -p "$ROOT"
[ "$RACH2_ARCH" = x86_64 ] || exit 0
archive=$(asset ge-proton)
# The release's published SHA-512 is kept beside the SHA-256 pin.
want=$(sed -n 's/^# ge-proton sha512: //p' "$GNU/sources-extra.txt")
got=$(sha512sum "$archive" | awk '{print $1}')
[ -n "$want" ] && [ "$got" = "$want" ] || {
    echo "gnu: proton: refused: release sha512 $got, expected $want" >&2
    exit 1
}
mkdir -p "$ROOT/opt/proton"
tar -xzf "$archive" -C "$ROOT/opt/proton" --strip-components=1
[ -x "$ROOT/opt/proton/proton" ] && [ -d "$ROOT/opt/proton/files" ] || {
    echo "gnu: proton: release has no launcher or runtime" >&2
    exit 1
}
echo "gnu: proton: result: GE-Proton11-12 verified and staged"
