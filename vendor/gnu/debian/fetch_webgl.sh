#!/bin/sh
# Stage the pinned Khronos WebGL suite without fetching at run time.

set -e
GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
archive=$(asset webgl)
ROOT="$GNU_OUT/webgl"
mkdir -p "$ROOT/usr/share/webgl"
tar -xzf "$archive" -C "$ROOT/usr/share/webgl" --strip-components=1
[ -f "$ROOT/usr/share/webgl/sdk/tests/webgl-conformance-tests.html" ] || {
    echo "gnu: webgl: conformance harness missing" >&2
    exit 1
}
echo "gnu: webgl: result: pinned conformance suite staged"
