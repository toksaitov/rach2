#!/bin/sh
#
# build_libnl.sh: build libnl's libnl-3 and libnl-genl-3 as static libraries
# for the musl userland, which wpa_supplicant, hostapd and iw link for
# nl80211, from the pinned, vendored tarball.
#
# The ports compile against the Linux UAPI headers libnl keeps in
# include/linux-private/ and the few more in linux-uapi/ beside this script,
# which musl does not install, and never the build host's own: a host
# toolchain for Linux has them and the bare i686-elf one has none. Both sets
# are staged with libnl's headers, for wpa_supplicant, hostapd and iw.
#
# configure insists on a pkg-config of at least 0.9.0, only to look for the
# unit tests' check library; the stand-in written to the work directory
# answers that question and finds no package, so the tests are not built and
# the build host needs no pkg-config. libnl-route-3 is not built: no program
# configured here links it, and its parsers need bison and flex on the host.
# The headers and the two archives are staged under build/vendor/libnl/ or
# build/x86_64/vendor/libnl/, with COPYING; a program built against them
# compiles with -I STAGE/include and links -lnl-genl-3 -lnl-3.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_libnl.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"

VERSION="3.12.0"
TARBALL="$SCRIPT_DIR/libnl-$VERSION.tar.gz"
# The digest the release publishes beside the tarball on GitHub.
TARBALL_SHA256="fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18"
WORK="$ARCH_BUILD/vendor-work/libnl"
SRC="$WORK/libnl-$VERSION"
STAGE="$ARCH_BUILD/vendor/libnl"
WRAP="$REPO/vendor/musl/muslgcc"

if [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
    echo "libnl: $MUSL_PREFIX/lib/libc.a not found; run make musl first" >&2
    exit 1
fi
GOT="$("$REPO/build/tools/verify/sha256" "$TARBALL" | awk '{print $1}')"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "libnl: sha256 mismatch for $TARBALL" >&2
    exit 1
fi

echo "libnl: extracting into $WORK"
rm -rf "$SRC" "$STAGE"
mkdir -p "$WORK"
tar xzf "$TARBALL" -C "$WORK"
printf '#!/bin/sh\n[ "$1" = --atleast-pkgconfig-version ]\n' > "$WORK/pkg-config"
chmod +x "$WORK/pkg-config"

echo "libnl: configuring"
BUILD_TRIPLE="$(sh "$SRC/build-aux/config.guess")"
(
    cd "$SRC"
    CC="$WRAP" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" PKG_CONFIG="$WORK/pkg-config" \
        CFLAGS="-Os -ffunction-sections -fdata-sections -I$SCRIPT_DIR/linux-uapi" \
        ./configure --build="$BUILD_TRIPLE" --host="$HOST_TRIPLE" --prefix=/usr \
        --disable-shared --enable-static --disable-cli --disable-pthreads \
        --disable-debug > "$WORK/configure.log" 2>&1 || {
        tail -20 "$WORK/configure.log" >&2
        exit 1
    }
)

echo "libnl: building libnl-3 and libnl-genl-3"
make -C "$SRC" lib/libnl-3.la lib/libnl-genl-3.la > "$WORK/build.log" 2>&1 || {
    tail -40 "$WORK/build.log" >&2
    exit 1
}

mkdir -p "$STAGE/lib" "$STAGE/include"
cp -R "$SRC/include/netlink" "$SRC/include/linux-private/." "$SCRIPT_DIR/linux-uapi/." \
    "$STAGE/include/"
cp "$SRC/lib/.libs/libnl-3.a" "$SRC/lib/.libs/libnl-genl-3.a" "$STAGE/lib/"
cp "$SRC/COPYING" "$STAGE/COPYING"
echo "libnl: result: built $VERSION for $RACH2_ARCH"
