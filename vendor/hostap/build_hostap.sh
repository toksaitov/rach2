#!/bin/sh
#
# build_hostap.sh: build wpa_supplicant, wpa_cli and wpa_passphrase, and
# hostapd and hostapd_cli, static for the musl userland, from the pinned,
# vendored release tarballs of hostap 2.12.
#
# The project releases wpa_supplicant and hostapd as two tarballs of one
# tree, each with the shared src/ and its own program's directory. Each
# program is built in its own tarball's tree with the configuration beside
# this script, wpa_supplicant.config or hostapd.config, against the OpenSSL
# of vendor/openssl/ and the libnl of vendor/libnl/, which must be staged
# first. The compiler is the bare i686-elf or x86_64-elf one, which defines
# no __linux__; the sources choose Linux's byte-order headers and the Linux
# parts of their code by it, and Rach2 offers Linux's interface, so it is
# defined on the command line instead of patched in.
#
# wpa_passphrase computes only PBKDF2-SHA1 and is built first with the
# project's internal SHA-1 and no driver, 34 KB instead of the 2 MB a link
# with OpenSSL makes; the tree is then cleaned for the build with OpenSSL.
#
# The stripped programs and COPYING are staged under build/vendor/hostap/ or
# build/x86_64/vendor/hostap/.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_hostap.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"

VERSION="2.12"
# The digests of the tarballs whose signatures, made with Jouni Malinen's key
# EC4AA0A991A5F2464582D52D2B6EF432EFC895FA, gpg verified.
WPA_SHA256="08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6"
HOSTAPD_SHA256="f43502561c28ba47ab77e18e1a973d07361c68cc8b14178e619bd5796b70eabd"
WORK="$ARCH_BUILD/vendor-work/hostap"
STAGE="$ARCH_BUILD/vendor/hostap"
OPENSSL="$ARCH_BUILD/vendor/openssl"
LIBNL="$ARCH_BUILD/vendor/libnl"
WRAP="$REPO/vendor/musl/muslgcc"

for f in "$OPENSSL/lib/libcrypto.a" "$LIBNL/lib/libnl-genl-3.a"; do
    if [ ! -f "$f" ]; then
        echo "hostap: $f not found; run make openssl libnl first" >&2
        exit 1
    fi
done

# extract NAME DIGEST: check NAME-VERSION.tar.gz and extract it into WORK.
extract()
{
    got="$("$REPO/build/tools/verify/sha256" "$SCRIPT_DIR/$1-$VERSION.tar.gz" | awk '{print $1}')"
    if [ "$got" != "$2" ]; then
        echo "hostap: sha256 mismatch for $1-$VERSION.tar.gz" >&2
        exit 1
    fi
    rm -rf "$WORK/$1-$VERSION"
    tar xzf "$SCRIPT_DIR/$1-$VERSION.tar.gz" -C "$WORK"
}

# build DIR LOG ARGS...: make ARGS in DIR with the musl wrapper, the log in
# WORK/LOG and its tail on a failure.
build()
{
    dir="$1"
    log="$WORK/$2"
    shift 2
    make -C "$dir" CC="$WRAP" "$@" > "$log" 2>&1 || {
        tail -40 "$log" >&2
        exit 1
    }
}

echo "hostap: extracting into $WORK"
rm -rf "$STAGE"
mkdir -p "$WORK" "$STAGE"
extract wpa_supplicant "$WPA_SHA256"
extract hostapd "$HOSTAPD_SHA256"
WPA="$WORK/wpa_supplicant-$VERSION/wpa_supplicant"
HOSTAPD="$WORK/hostapd-$VERSION/hostapd"

CFLAGS_ALL="-D__linux__ -Os -ffunction-sections -fdata-sections"
LDFLAGS_ALL="-static -Wl,--gc-sections -Wl,-z,noexecstack"

echo "hostap: building wpa_passphrase with the internal SHA-1"
cp "$SCRIPT_DIR/wpa_supplicant.config" "$WPA/.config"
build "$WPA" passphrase.log CONFIG_TLS=internal CONFIG_INTERNAL_LIBTOMMATH=y \
    CONFIG_DRIVER_NL80211= CONFIG_LIBNL32= \
    EXTRA_CFLAGS="$CFLAGS_ALL" LDFLAGS="$LDFLAGS_ALL" wpa_passphrase
"${CROSS}strip" "$WPA/wpa_passphrase" -o "$STAGE/wpa_passphrase"
build "$WPA" clean.log clean

echo "hostap: building wpa_supplicant and wpa_cli"
FLAGS="EXTRA_CFLAGS=$CFLAGS_ALL -I$OPENSSL/include -I$LIBNL/include"
LINK="LDFLAGS=$LDFLAGS_ALL -L$OPENSSL/lib -L$LIBNL/lib"
build "$WPA" wpa_supplicant.log LIBNL_INC="$LIBNL/include" "$FLAGS" "$LINK" \
    wpa_supplicant wpa_cli
for p in wpa_supplicant wpa_cli; do
    "${CROSS}strip" "$WPA/$p" -o "$STAGE/$p"
done

echo "hostap: building hostapd and hostapd_cli"
cp "$SCRIPT_DIR/hostapd.config" "$HOSTAPD/.config"
build "$HOSTAPD" hostapd.log LIBNL_INC="$LIBNL/include" "$FLAGS" "$LINK" \
    hostapd hostapd_cli
for p in hostapd hostapd_cli; do
    "${CROSS}strip" "$HOSTAPD/$p" -o "$STAGE/$p"
done

cp "$WORK/wpa_supplicant-$VERSION/COPYING" "$STAGE/COPYING"
ls -l "$STAGE"
echo "hostap: result: built $VERSION for $RACH2_ARCH"
