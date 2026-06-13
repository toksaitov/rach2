#!/usr/bin/env sh

#
# build.sh - build the musl-linked test programs in this directory.
#
# These differ from the freestanding probes in tests/: they are ordinary hosted
# C programs linked statically against the ported musl (see libc/build_libc.sh).
# Each <name>.c here becomes a guest binary <name>, installed into the RAM disk
# with an "m" prefix by krffs/build_test_ramdisk.sh (e.g. hello -> mhello).
#
# Usage: ./build.sh        (run libc/build_libc.sh first if install/ is absent)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PREFIX="$SCRIPT_DIR/../../libc/install"
CC="i686-elf-gcc"
LIBGCC="$($CC -print-libgcc-file-name)"

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "error: $PREFIX/lib/libc.a not found; run libc/build_libc.sh first" >&2
    exit 1
fi

for src in *.c; do
    prog="${src%.c}"
    echo "  CC $prog"
    $CC -static -nostdlib -nostartfiles -isystem "$PREFIX/include" -O2 \
        -o "$prog" \
        "$PREFIX/lib/crt1.o" "$PREFIX/lib/crti.o" "$src" \
        "$PREFIX/lib/libc.a" "$LIBGCC" "$PREFIX/lib/crtn.o" \
        -Wl,-z,noexecstack
done

echo "Done."
