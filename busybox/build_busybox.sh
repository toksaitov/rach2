#!/usr/bin/env sh

#
# build_busybox.sh: build a static musl BusyBox for Rach2 from the pinned,
# vendored tarball (Phase 18). Verifies the sha256, extracts, applies the curated
# rach2.config, builds against the ported musl via the muslgcc wrapper, strips,
# and stages the binary to ../tests/busybox (installed into the RAM disk as
# /busybox by krffs/build_test_ramdisk.sh). Run libc/build_libc.sh first.
#
# Usage: ./build_busybox.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="1.36.1"
TARBALL="busybox-${VERSION}.tar.bz2"
SRC="busybox-${VERSION}"
PREFIX="$SCRIPT_DIR/../libc/install"
WRAP="$SCRIPT_DIR/muslgcc"

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
    echo "error: $PREFIX/lib/libc.a not found; run libc/build_libc.sh first" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "error: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "=== Verifying $TARBALL ==="
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "error: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "=== Extracting ==="
rm -rf "$SRC"
tar xjf "$TARBALL"

echo "=== Configuring (rach2.config: static musl ash + coreutils) ==="
cp rach2.config "$SRC/.config"
# Resolve any new/derived symbols against the pinned config, taking defaults.
yes "" | make -C "$SRC" oldconfig HOSTCC=cc >/dev/null

echo "=== Building (CC=muslgcc, CROSS_COMPILE=i686-elf-, HOSTCC=cc) ==="
make -C "$SRC" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
    CROSS_COMPILE=i686-elf- CC="$WRAP" HOSTCC=cc SKIP_STRIP=y busybox

echo "=== Staging stripped binary -> ../tests/busybox ==="
i686-elf-strip "$SRC/busybox" -o ../tests/busybox
ls -l ../tests/busybox

echo ""
echo "=== Done ==="
echo "BusyBox $VERSION: $(wc -c < ../tests/busybox) bytes, static, musl, i686."
echo "Rebuild the RAM disk (krffs/build_test_ramdisk.sh) to install it as /busybox."
