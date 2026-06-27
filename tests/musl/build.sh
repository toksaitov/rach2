#!/usr/bin/env sh

#
# Build the musl-linked test programs in this directory.
#
# These differ from the freestanding probes in tests/: they are ordinary hosted
# C programs linked statically against the ported musl (see vendor/musl/build_libc.sh).
# Each <name>.c here becomes a guest binary build/tests/musl/<name> (out of tree, like
# every other component), staged into the test image under /musl by its manifest (so
# /musl/hello, /musl/mallocqsort, ...). `make musl-programs` runs this.
#
# Run vendor/musl/build_libc.sh first if install/ is absent.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PREFIX="$SCRIPT_DIR/../../vendor/musl/install"
CC="i686-elf-gcc"
LIBGCC="$($CC -print-libgcc-file-name)"

# Out of tree, mirroring the source path tests/musl/ (mk/build.mk).
OUT="$(cd "$SCRIPT_DIR/../.." && pwd)/build/tests/musl"

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "error: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi

mkdir -p "$OUT"
for src in *.c; do
    prog="${src%.c}"
    echo "  CC $prog"
    $CC -static -nostdlib -nostartfiles -isystem "$PREFIX/include" -O2 \
        -o "$OUT/$prog" \
        "$PREFIX/lib/crt1.o" "$PREFIX/lib/crti.o" "$src" \
        "$PREFIX/lib/libc.a" "$LIBGCC" "$PREFIX/lib/crtn.o" \
        -Wl,-z,noexecstack
done
