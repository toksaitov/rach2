#!/bin/sh
#
# build_openssl.sh: build OpenSSL's libcrypto and libssl as static libraries
# for the musl userland, which wpa_supplicant and hostapd link
# (vendor/hostap/), from the tarball the GNU layer pins.
#
# The tarball is 55 MB, too large to keep in Git, so it is the GNU layer's
# download: vendor/gnu/fetch.sh fetches it into vendor/gnu/downloads/ once and
# checks it against vendor/gnu/sources.txt, and this script checks it again
# against its own pin below, so that a change of the GNU layer's version
# stops this build instead of changing it.
#
# Configure runs with perl. The compiler is the musl wrapper, found on the
# path by its bare name, since libcrypto keeps the compiler's command line
# for OpenSSL_version() and an absolute path there would name this copy. The
# libraries are built without threads, shared objects, position-independent
# code, loadable modules or the command-line program, with the legacy
# provider (MD4, DES and RC4, which MSCHAPv2 and WPA's TKIP need) built into
# libcrypto, and without the algorithms and protocols hostap never calls,
# since a provider's table of algorithms links every one it has into a
# static program. The headers and the two archives are staged under
# build/vendor/openssl/ or build/x86_64/vendor/openssl/, with LICENSE.txt.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_openssl.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"

VERSION="4.0.2"
# The digest vendor/gnu/sources.txt pins, after its signature was checked.
TARBALL_SHA256="736b467530f916737b7031310ccb21d8218c6229e61e8e160cd1d3458cd543a8"
WORK="$ARCH_BUILD/vendor-work/openssl"
SRC="$WORK/openssl-$VERSION"
STAGE="$ARCH_BUILD/vendor/openssl"
WRAP="$REPO/vendor/musl/muslgcc"

case "$RACH2_ARCH" in
    i686) TARGET=linux-x86 ;;
    x86_64) TARGET=linux-x86_64 ;;
esac

if [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
    echo "openssl: $MUSL_PREFIX/lib/libc.a not found; run make musl first" >&2
    exit 1
fi
if ! command -v perl > /dev/null 2>&1; then
    echo "openssl: perl not found; OpenSSL's Configure needs it" >&2
    exit 1
fi

TARBALL="$(sh "$REPO/vendor/gnu/fetch.sh" --path openssl)"
case "$TARBALL" in
    */openssl-$VERSION.tar.gz) ;;
    *)
        echo "openssl: the GNU layer pins $(basename "$TARBALL"), not $VERSION;" \
            "change VERSION and TARBALL_SHA256 here to match" >&2
        exit 1
        ;;
esac
GOT="$("$REPO/build/tools/verify/sha256" "$TARBALL" | awk '{print $1}')"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "openssl: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "openssl: extracting into $WORK"
rm -rf "$SRC" "$WORK/destdir" "$STAGE"
mkdir -p "$WORK"
tar xzf "$TARBALL" -C "$WORK"

# The options after the build's own (no shared objects, modules, threads,
# async jobs, DSO loader, program, documents or tests) leave out what hostap
# does not use: QUIC, DTLS, kernel TLS and the AF_ALG engine; CMS, CMP,
# certificate transparency, time stamps and SRP; the national and the
# retired ciphers and digests; the post-quantum signatures; binary curves;
# compression; and the console UI that would ask for a key's passphrase.
echo "openssl: configuring $TARGET"
(
    cd "$SRC"
    PATH="$(dirname "$WRAP"):$PATH" CC=muslgcc AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
        perl ./Configure "$TARGET" --prefix=/usr --libdir=lib --openssldir=/etc/ssl \
        no-shared no-pic no-module no-threads no-async no-dso no-apps no-docs \
        no-tests no-uplink no-secure-memory \
        no-quic no-dtls no-srtp no-sctp no-ktls no-afalgeng \
        no-cms no-cmp no-ct no-ts no-srp no-rfc3779 no-ech no-ssl-trace \
        no-sm2 no-sm2-precomp no-sm3 no-sm4 no-gost no-idea no-seed no-camellia \
        no-aria no-bf no-cast no-rc2 no-rc5 no-mdc2 no-whirlpool no-siphash \
        no-scrypt no-argon2 no-siv no-ocb no-integrity-only-ciphers \
        no-ml-dsa no-slh-dsa no-lms no-ec2m no-comp no-ui-console \
        -Os -ffunction-sections -fdata-sections > "$WORK/configure.log" 2>&1 || {
        tail -20 "$WORK/configure.log" >&2
        exit 1
    }
)

echo "openssl: building libcrypto and libssl"
PATH="$(dirname "$WRAP"):$PATH"
make -C "$SRC" build_libs > "$WORK/build.log" 2>&1 || {
    tail -40 "$WORK/build.log" >&2
    exit 1
}
make -C "$SRC" DESTDIR="$WORK/destdir" install_dev > "$WORK/install.log" 2>&1 || {
    tail -20 "$WORK/install.log" >&2
    exit 1
}

mkdir -p "$STAGE/lib"
cp -R "$WORK/destdir/usr/include" "$STAGE/include"
cp "$WORK/destdir/usr/lib/libcrypto.a" "$WORK/destdir/usr/lib/libssl.a" "$STAGE/lib/"
cp "$SRC/LICENSE.txt" "$STAGE/LICENSE.txt"
echo "openssl: result: built $VERSION for $RACH2_ARCH"
