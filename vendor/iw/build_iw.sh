#!/bin/sh
#
# build_iw.sh: build iw, the nl80211 configuration program, static for the
# musl userland from the pinned, vendored tarball, against the libnl of
# vendor/libnl/, which must be staged first.
#
# iw's Makefile asks pkg-config for libnl unless NO_PKG_CONFIG is set; here it
# is set and the flags are given instead. CFLAGS comes from the environment,
# since the Makefile appends its warnings and EXTRA_CFLAGS to it and a CFLAGS
# on make's command line would replace them. __linux__ is defined for the
# reason build_hostap.sh gives. The Makefile makes nl80211-commands.inc with
# GNU sed's \L, which BSD sed lacks, so this script writes it first with awk;
# make then finds it newer than nl80211.h and keeps it.
#
# The stripped program and COPYING are staged under build/vendor/iw/ or
# build/x86_64/vendor/iw/.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_iw.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"

VERSION="6.17"
TARBALL="$SCRIPT_DIR/iw-$VERSION.tar.xz"
# The digest of the tarball whose signature, made with Johannes Berg's key
# A5E03CB13B3733C48DDA1478D74AA23BCB0F6800, gpg verified on the uncompressed
# tar, as kernel.org signs it.
TARBALL_SHA256="7d182e498289ab39b257da6780d562e415377107f50358ee5b55b8cfe40b1e33"
WORK="$ARCH_BUILD/vendor-work/iw"
SRC="$WORK/iw-$VERSION"
STAGE="$ARCH_BUILD/vendor/iw"
LIBNL="$ARCH_BUILD/vendor/libnl"
WRAP="$REPO/vendor/musl/muslgcc"

if [ ! -f "$LIBNL/lib/libnl-genl-3.a" ]; then
    echo "iw: $LIBNL/lib/libnl-genl-3.a not found; run make libnl first" >&2
    exit 1
fi
GOT="$("$REPO/build/tools/verify/sha256" "$TARBALL" | awk '{print $1}')"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "iw: sha256 mismatch for $TARBALL" >&2
    exit 1
fi

echo "iw: extracting into $WORK"
rm -rf "$SRC" "$STAGE"
mkdir -p "$WORK" "$STAGE"
tar -xf "$TARBALL" -C "$WORK"

# Each line `\tNL80211_CMD_NAME,` of nl80211.h becomes
# `\t[NL80211_CMD_NAME] = "name",`, as the Makefile's sed makes it: a line
# with an `=` before its comma, an alias or the maximum, is left out, and so
# is a name with "reserved" in it.
awk '/^\tNL80211_CMD_[^=]*,/ {
    sub(/^\tNL80211_CMD_/, ""); sub(/,.*/, "")
    if (tolower($0) ~ /reserved/) next
    printf "\t[NL80211_CMD_%s] = \"%s\",\n", $0, tolower($0)
}' "$SRC/nl80211.h" > "$SRC/nl80211-commands.inc"

echo "iw: building"
(
    cd "$SRC"
    CFLAGS="-Os -ffunction-sections -fdata-sections" \
        make NO_PKG_CONFIG=1 CC="$WRAP" \
        EXTRA_CFLAGS="-D__linux__ -DCONFIG_LIBNL30 -I$LIBNL/include" \
        LDFLAGS="-static -Wl,--gc-sections -Wl,-z,noexecstack -L$LIBNL/lib" \
        LIBS="-lnl-genl-3 -lnl-3" iw
)
"${CROSS}strip" "$SRC/iw" -o "$STAGE/iw"
cp "$SRC/COPYING" "$STAGE/COPYING"
ls -l "$STAGE/iw"
echo "iw: result: built $VERSION for $RACH2_ARCH"
