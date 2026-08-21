#!/usr/bin/env sh

#
# build_dropbear.sh: build a static-musl Dropbear SSH server for Rach2 from the
# pinned, vendored tarball. Verifies the sha256, extracts, configures
# against the ported musl via the muslgcc wrapper (bundled libtomcrypt/libtommath,
# no zlib/PAM/utmp/syslog/loginrec, hardening OFF so the binary links static
# non-PIE; Rach2's ELF loader does not relocate a PIE), applies the curated
# localoptions.h, builds dropbearmulti (the dropbear server, the dbclient SSH
# client, and dropbearkey, dispatched on argv[1], so `dropbearmulti dropbear`),
# strips, and stages it out of tree
# to build/vendor/dropbear/dropbearmulti (staged at /bin/dropbearmulti in the os-real
# image by its manifest). Run vendor/musl/build_libc.sh first.
#
# Usage: ./build_dropbear.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="2022.83"
TARBALL="dropbear-${VERSION}.tar.bz2"
SRC="dropbear-${VERSION}"
PREFIX="$SCRIPT_DIR/../musl/install"
WRAP="$SCRIPT_DIR/muslgcc"
# Out-of-tree staging dir, mirroring the source path vendor/dropbear/ (mk/build.mk).
# It lives under build/ so a routine `make clean` never reaches it; only `make
# distclean` (which removes build/vendor) forces this multi-minute rebuild.
STAGE="$(cd "$SCRIPT_DIR/../.." && pwd)/build/vendor/dropbear"

# Pinned sha256 of the vendored tarball (published on matt.ucc.asn.au/dropbear).
TARBALL_SHA256="bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b"

sha256_of()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "dropbear: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "dropbear: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "dropbear: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "dropbear: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "dropbear: extracting"
rm -rf "$SRC"
tar xjf "$TARBALL"

# Apply any curated patches, in order, before building. The vendored tree is
# otherwise pristine; this is the escape hatch for the rare fix localoptions.h
# cannot express. With no patches/ directory the loop does nothing.
if [ -d patches ]; then
    for p in patches/*.patch; do
        [ -e "$p" ] || break
        echo "dropbear: applying $p"
        patch -p1 -d "$SRC" < "$p"
    done
fi

echo "dropbear: installing the curated localoptions.h"
cp localoptions.h "$SRC/localoptions.h"

echo "dropbear: configuring (muslgcc, bundled libtom, static, hardening off)"
# --build is THIS host's own triple. config.guess ships with dropbear (not +x in the
# tarball, so run it through sh); computing the triple (instead of hard-coding
# x86_64-apple-darwin) makes the cross-configure work on Linux and on Intel or
# Apple-Silicon macOS alike. If config.guess fails, omit --build and let configure
# infer it.
BUILD_TRIPLE="$(sh "$SRC/config.guess" 2>/dev/null || true)"
(
    cd "$SRC"
    set -- --host=i686-linux-musl
    [ -n "$BUILD_TRIPLE" ] && set -- "$@" --build="$BUILD_TRIPLE"
    CC="$WRAP" AR=i686-elf-ar RANLIB=i686-elf-ranlib \
        ./configure "$@" \
        --disable-zlib --enable-bundled-libtom --disable-syslog --disable-harden \
        --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
        --disable-lastlog --disable-loginfunc --disable-pututline --disable-pututxline \
        >/dev/null
)

echo "dropbear: building dropbearmulti (dropbear server + dbclient + dropbearkey, static)"
make -C "$SRC" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
    PROGRAMS="dropbear dbclient dropbearkey" MULTI=1 STATIC=1 SKIP_STRIP=y dropbearmulti

echo "dropbear: staging the stripped binary to build/vendor/dropbear/dropbearmulti"
mkdir -p "$STAGE"
i686-elf-strip "$SRC/dropbearmulti" -o "$STAGE/dropbearmulti"
ls -l "$STAGE/dropbearmulti"

echo "dropbear: built $VERSION ($(wc -c < "$STAGE/dropbearmulti") bytes, static, musl, i686)"
echo "dropbear: rebuild the os-real image (make os-real-image) to install it"
