#!/bin/sh
#
# build_weston.sh: rebuild Debian's Weston 14.0.2 with Rach2's patches in the
# Debian 13 builder guest, for the x86-64 GNU root's games route.
#
# Usage: build_weston.sh
#        build_weston.sh guest
#
# On the host, the script fetches the source package that sources.txt pins
# and checks each file's SHA-256, prepares the builder guest of
# tests/graphics/debian-ref if this clone has none, and boots it with the
# source package, the patches that `series` lists and itself, which the guest
# runs as `build_weston.sh guest`. There dpkg-source unpacks the package, the
# patches join debian/patches, a changelog entry names the version VERSION
# and the patches, and dpkg-buildpackage builds the binary packages with
# Debian's own rules and flags, without its tests. The changelog entry's date
# is fixed, so that the packages' timestamps, which SOURCE_DATE_EPOCH takes
# from it, do not change from build to build.
#
# The host checks the SHA-256 of the two packages the root takes, weston and
# libweston-14-0, against `debs.sha256`, and unpacks their data into
# weston/ of the GNU build tree, which install_games.sh puts in place of the
# desktop closure's files of the same packages. The packages themselves, the
# build's log and the guest's record are in work/weston. GNU_WESTON_CPUS and
# GNU_WESTON_MIB give the guest's processors and memory, 8 and 8192.
#

set -e

VERSION=14.0.2-1+rach2.1
# The changelog entry's date, which every rebuild of VERSION keeps.
DATE='Tue, 10 Nov 2026 12:00:00 +0600'

if [ "${1:-}" = guest ]; then
    set -u
    say()
    {
        echo "weston: $*"
    }
    # Debian's /tmp is a tmpfs of half the memory; the build path is fixed,
    # since the packages' debug links and paths include it.
    work=/var/tmp/weston
    rm -rf "$work"
    mkdir -p "$work"
    cd "$work"
    say "unpacking weston 14.0.2-1"
    dpkg-source -x /tmp/weston_14.0.2-1.dsc src
    cd src
    mkdir -p debian/patches
    : > debian/patches/series
    while read -r patch; do
        case $patch in '' | '#'*) continue ;; esac
        cp "/tmp/$patch" debian/patches/
        echo "$patch" >> debian/patches/series
    done < /tmp/series
    {
        echo "weston ($VERSION) trixie; urgency=medium"
        echo
        echo "  * Rach2's rebuild for the games route, with these patches:"
        sed -e '/^#/d' -e '/^$/d' -e 's/^/    - /' debian/patches/series
        echo
        # The trailer opens with a space and two hyphens (deb-changelog(5)).
        printf ' %s Rach2 <rach2@localhost>  %s\n' '-''-' "$DATE"
        echo
        cat debian/changelog
    } > debian/changelog.new
    mv debian/changelog.new debian/changelog
    say "installing the build dependencies"
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y build-dep ./
    DEB_BUILD_OPTIONS="parallel=$(nproc) nocheck"
    export DEB_BUILD_OPTIONS
    say "building $VERSION"
    dpkg-buildpackage -b -uc -us
    cd "$work"
    for p in gcc meson libwayland-dev wayland-protocols libxcb1-dev xwayland; do
        say "version: $p $(dpkg-query -W -f '${Version}' "$p")"
    done
    say "flags: CFLAGS $(dpkg-buildflags --get CFLAGS)"
    mkdir out
    for p in weston libweston-14-0; do
        cp "${p}_${VERSION}_amd64.deb" out/
    done
    sha256sum out/*.deb | sed 's|out/||' | sed 's/^/weston: sha256: /'
    tar -c -f /tmp/weston.tar -C out .
    say "result: built $VERSION"
    exit 0
fi

GAMES=$(cd "$(dirname "$0")/.." && pwd)
GNU=$(cd "$GAMES/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
if [ "$RACH2_ARCH" != x86_64 ]; then
    echo "gnu: weston: refused: the rebuilt Weston is the x86-64 root's" >&2
    exit 1
fi
HERE="$GAMES/weston"
ROOT="$GNU_OUT/weston"
WORK="$GNU_OUT/work/weston"
rm -rf "$ROOT" "$WORK"
mkdir -p "$ROOT" "$WORK/debs"
pins="$HERE/sources.txt"
dsc=$(asset weston-dsc "$pins")
orig=$(asset weston-orig "$pins")
asc=$(asset weston-orig-asc "$pins")
debian=$(asset weston-debian "$pins")
set -- -i "$dsc" -i "$orig" -i "$asc" -i "$debian" -i "$HERE/series" -i "$HERE/build_weston.sh"
while read -r patch; do
    case $patch in '' | '#'*) continue ;; esac
    set -- "$@" -i "$HERE/$patch"
done < "$HERE/series"
ref="$REPO/tests/graphics/debian-ref/debian-ref.sh"
sh "$ref" prepare -n
echo "gnu: weston: building in the builder guest; the log is $WORK/build.log"
if ! sh "$ref" run -c "${GNU_WESTON_CPUS:-8}" -m "${GNU_WESTON_MIB:-8192}" -r "$WORK/guest.record" \
    "$@" -o "$WORK/weston.tar" 'sh /tmp/build_weston.sh guest' > "$WORK/build.log" 2>&1; then
    tail -n 40 "$WORK/build.log" >&2
    echo "gnu: weston: the build in the builder guest failed; the log is $WORK/build.log" >&2
    exit 1
fi
tar -x -f "$WORK/weston.tar" -C "$WORK/debs"
rm -f "$WORK/weston.tar"
make -s -C "$REPO" build/tools/verify/sha256
while read -r want deb; do
    case $want in '' | '#'*) continue ;; esac
    got=$("$REPO/build/tools/verify/sha256" "$WORK/debs/$deb" | awk '{print $1}')
    if [ "$got" != "$want" ]; then
        echo "gnu: weston: refused: $deb: sha256 $got, debs.sha256 has $want" >&2
        exit 1
    fi
    member=$(ar t "$WORK/debs/$deb" | grep '^data\.tar\.') || {
        echo "gnu: weston: $deb has no data archive" >&2
        exit 1
    }
    ar p "$WORK/debs/$deb" "$member" | xz -dc | tar -x -f - -C "$ROOT"
    echo "gnu: weston: staged $deb"
done < "$HERE/debs.sha256"
echo "gnu: weston: result: $(grep '^weston: result: ' "$WORK/build.log" | sed 's/^weston: result: //')"
