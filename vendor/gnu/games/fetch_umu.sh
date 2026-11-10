#!/bin/sh
# Stage umu-launcher and the Steam Linux Runtime archive it runs Proton in,
# both checked against umu.list, under /opt/umu for x86-64, with the
# archive's VERSIONS.txt beside it; i686 has no Proton layer.

set -e
GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
ROOT="$GNU_OUT/umu"
LIST="$GNU/games/umu.list"
mkdir -p "$ROOT"
[ "$RACH2_ARCH" = x86_64 ] || exit 0

# The pinned version of NAME in umu.list.
pin()
{
    awk -v n="$1" '$1 == n { print $2; exit }' "$LIST"
}

zipapp=$(asset umu-launcher "$LIST")
runtime=$(asset steamrt4 "$LIST")
mkdir -p "$ROOT/opt/umu"
tar -xf "$zipapp" -C "$ROOT/opt/umu" --strip-components=1 umu/umu-run
# The zipapp is a shell line before a zip archive, which Python's zipfile reads.
version=$(python3 -c 'import re, sys, zipfile
for n in zipfile.ZipFile(sys.argv[1]).namelist():
    m = re.fullmatch(r"umu_launcher-([^/]*)\.dist-info/METADATA", n)
    if m:
        print(m.group(1))' "$ROOT/opt/umu/umu-run")
[ "$version" = "$(pin umu-launcher)" ] || {
    echo "gnu: umu: refused: umu-run is version '$version', expected $(pin umu-launcher)" >&2
    exit 1
}
chmod 0755 "$ROOT/opt/umu/umu-run"

# VERSIONS.txt is the archive's first file; the runtime's own line must
# name the pinned build, so that the pin and the image directory agree.
# rach2-game compares its copy beside the archive with a user's runtime.
tar -xJOf "$runtime" --occurrence=1 SteamLinuxRuntime_4/VERSIONS.txt \
    > "$ROOT/opt/umu/VERSIONS.txt"
built=$(awk '$1 == "steamrt4" { print $2 }' "$ROOT/opt/umu/VERSIONS.txt")
[ "$built" = "$(pin steamrt4)" ] || {
    echo "gnu: umu: refused: the runtime is steamrt4 '$built', expected $(pin steamrt4)" >&2
    exit 1
}
cp "$runtime" "$ROOT/opt/umu/SteamLinuxRuntime_4.tar.xz"
chmod 0644 "$ROOT/opt/umu/SteamLinuxRuntime_4.tar.xz" "$ROOT/opt/umu/VERSIONS.txt"
echo "gnu: umu: result: umu-launcher $version and Steam Linux Runtime $built verified and staged"
