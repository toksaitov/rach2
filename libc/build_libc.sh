#!/usr/bin/env sh

#
# build_libc.sh
#
# Builds musl libc for Rach2 from the pinned, vendored source tarball using the
# stock i686-elf-gcc cross compiler (no separate musl cross-toolchain is
# needed, since students already install i686-elf-gcc for the kernel). Produces a
# static libc.a plus the crt1/crti/crtn startup objects and the musl headers
# under install/, which Rach2 utilities link against (see Readme.md).
#
# Usage:
#     ./build_libc.sh
#
# Without arguments it verifies, extracts, configures, builds, and installs.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MUSL_VERSION="1.2.5"
TARBALL="musl-${MUSL_VERSION}.tar.gz"
SRC="musl-${MUSL_VERSION}"
PREFIX="$SCRIPT_DIR/install"

# Pinned sha256 of the vendored tarball (published on musl.libc.org). The build
# refuses to proceed if the tarball does not match; the version is locked.
SHA256="a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"

# sha256 helper that works on both macOS (shasum) and Linux (sha256sum).
sha256_of()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

echo "=== Verifying pinned musl tarball ==="
if [ ! -f "$TARBALL" ]; then
    echo "Error: $TARBALL not found. It is vendored beside this script."
    exit 1
fi
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$SHA256" ]; then
    echo "Error: sha256 mismatch for $TARBALL"
    echo "  expected $SHA256"
    echo "  got      $GOT"
    exit 1
fi
echo "  OK ($SHA256)"

echo ""
echo "=== Extracting ==="
rm -rf "$SRC"
tar xzf "$TARBALL"

echo ""
echo "=== Configuring (CROSS_COMPILE=i686-elf-, static, target i686) ==="
cd "$SRC"
CROSS_COMPILE=i686-elf- ./configure --target=i686 --disable-shared --prefix="$PREFIX"

echo ""
echo "=== Building ==="
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

echo ""
echo "=== Installing to $PREFIX (cleaned first for a reproducible tree) ==="
rm -rf "$PREFIX"
make install

echo ""
echo "=== Done ==="
echo "  libc.a:  $PREFIX/lib/libc.a"
echo "  startup: $PREFIX/lib/crt1.o $PREFIX/lib/crti.o $PREFIX/lib/crtn.o"
echo "  headers: $PREFIX/include"
echo ""
echo "Link a program against it (see Readme.md for the full recipe):"
echo "  i686-elf-gcc -static -nostdlib -nostartfiles -isystem $PREFIX/include \\"
echo "    -o prog crt1.o crti.o prog.c libc.a \$(i686-elf-gcc -print-libgcc-file-name) \\"
echo "    crtn.o -Wl,-z,noexecstack"
