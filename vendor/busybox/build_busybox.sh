#!/usr/bin/env sh

#
# build_busybox.sh: build a static musl BusyBox for Rach2 from the pinned,
# vendored tarball. Verifies the sha256, extracts, applies the curated
# rach2.config, builds against the ported musl via the muslgcc wrapper, strips,
# and stages the binary out of tree to build/vendor/busybox/busybox (staged into the
# os-real RAM-disk image by its manifest). Run vendor/musl/build_libc.sh first.
#
# Usage: ./build_busybox.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="1.36.1"
TARBALL="busybox-${VERSION}.tar.bz2"
SRC="busybox-${VERSION}"
PREFIX="$SCRIPT_DIR/../musl/install"
WRAP="$SCRIPT_DIR/muslgcc"
# Out-of-tree staging dir, mirroring the source path vendor/busybox/ (mk/build.mk).
# It lives under build/ so a routine `make clean` never reaches it; only `make
# distclean` (which removes build/vendor) forces this multi-minute rebuild.
STAGE="$(cd "$SCRIPT_DIR/../.." && pwd)/build/vendor/busybox"

# Pinned sha256 of the vendored tarball (published on busybox.net). The build
# refuses to proceed if the tarball does not match; the version is locked.
TARBALL_SHA256="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"

sha256_of()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "busybox: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "busybox: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "busybox: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "busybox: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "busybox: extracting"
rm -rf "$SRC"
tar xjf "$TARBALL"

# Apply any curated patches, in order, before configuring. The vendored tree is
# otherwise pristine; this is the escape hatch for the rare fix the config cannot
# express. With no patches/ directory the loop does nothing.
if [ -d patches ]; then
    for p in patches/*.patch; do
        [ -e "$p" ] || break
        echo "busybox: applying $p"
        patch -p1 -d "$SRC" < "$p"
    done
fi

echo "busybox: configuring (rach2.config: static musl ash + coreutils)"
cp rach2.config "$SRC/.config"
# Resolve any new/derived symbols against the pinned config, taking defaults.
yes "" | make -C "$SRC" oldconfig HOSTCC=cc >/dev/null

echo "busybox: building (CC=muslgcc, CROSS_COMPILE=i686-elf-, HOSTCC=cc)"
make -C "$SRC" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
    CROSS_COMPILE=i686-elf- CC="$WRAP" HOSTCC=cc SKIP_STRIP=y busybox

echo "busybox: staging the stripped binary to build/vendor/busybox/busybox"
mkdir -p "$STAGE"
i686-elf-strip "$SRC/busybox" -o "$STAGE/busybox"
ls -l "$STAGE/busybox"

echo "busybox: built $VERSION ($(wc -c < "$STAGE/busybox") bytes, static, musl, i686)"
echo "busybox: rebuild the os-real image (make os-real-image) to install it"
