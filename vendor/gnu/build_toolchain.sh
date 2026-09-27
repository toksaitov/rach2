#!/usr/bin/env sh

#
# build_toolchain.sh: the i686-linux-gnu cross toolchain of the GNU layer.
#
# Usage: build_toolchain.sh [STEP...]
#
# Builds, from the tarballs fetch.sh checks, into build/vendor/gnu/: binutils,
# a first C-only GCC without a C library, the Linux headers, glibc, the final
# GCC with C and C++, and libxcrypt, whose libcrypt glibc 2.44 no longer has.
# The compilers go in cross/, the target's headers and libraries in sysroot/.
# The steps are binutils, headers, gcc1, glibc, gcc2 and libxcrypt; with no
# STEP the whole toolchain is built again from nothing, and naming steps runs
# only those, over what an earlier run left. Under make the sub-makes share
# the caller's jobs; run alone, the script uses every processor.
#

# shellcheck disable=SC2086 # JOBS and GCC_COMMON are lists of words
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
SRC="$OUT/src"
WORK="$OUT/work"
CROSS="$OUT/cross"
SYSROOT="$OUT/sysroot"
TARGET=i686-linux-gnu
PATH="$CROSS/bin:$PATH"
export PATH

case " $MAKEFLAGS " in
    *jobserver*) JOBS= ;;
    *) JOBS="-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" ;;
esac

for tool in cc c++ make gawk bison flex m4 python3 makeinfo xz patch; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: toolchain: $tool not found; the GNU layer builds on a Linux host with the tools vendor/gnu/Readme.md lists" >&2
        exit 1
    fi
done
BUILD="$(cc -dumpmachine)"

# extract NAME [DEST]: unpack a checked tarball afresh and print its tree.
extract()
{
    tarball="$("$DIR/fetch.sh" --path "$1")"
    dest="${2:-$SRC}"
    tree="$dest/$(basename "$tarball" | sed -e 's/\.tar\..*$//' -e 's/\.tgz$//')"
    mkdir -p "$dest"
    rm -rf "$tree"
    echo "gnu: toolchain: extracting $(basename "$tarball")" >&2
    tar -xf "$tarball" -C "$dest"
    echo "$tree"
}

# fresh NAME: an empty build directory for one step.
fresh()
{
    rm -rf "${WORK:?}/$1"
    mkdir -p "$WORK/$1"
    echo "$WORK/$1"
}

step_binutils()
{
    s="$(extract binutils)"
    cd "$(fresh binutils)"
    "$s/configure" --target="$TARGET" --prefix="$CROSS" --with-sysroot="$SYSROOT" \
        --disable-nls --disable-werror --disable-gprofng
    make $JOBS
    make install
}

step_headers()
{
    s="$(extract linux)"
    cd "$s"
    make ARCH=x86 INSTALL_HDR_PATH="$SYSROOT/usr" headers_install
}

# GCC builds GMP, MPFR and MPC in its own tree when their sources sit beside
# it under those names.
gcc_source()
{
    s="$(extract gcc)"
    for lib in gmp mpfr mpc; do
        t="$(extract "$lib" "$SRC/$lib-tree")"
        mv "$t" "$s/$lib"
    done
    rm -rf "$SRC/gmp-tree" "$SRC/mpfr-tree" "$SRC/mpc-tree"
    echo "$s"
}

GCC_COMMON="--target=$TARGET --prefix=$CROSS --with-sysroot=$SYSROOT \
    --with-arch=i686 --with-tune=generic --disable-multilib --disable-nls \
    --disable-bootstrap --disable-libgomp --disable-libitm --disable-libquadmath \
    --disable-libsanitizer --disable-libssp --disable-libvtv"

# The first compiler has no C library to build against: --with-newlib and
# --without-headers keep libgcc from asking for one.
step_gcc1()
{
    s="$(gcc_source)"
    cd "$(fresh gcc1)"
    "$s/configure" $GCC_COMMON --enable-languages=c --with-newlib --without-headers \
        --disable-shared --disable-threads --disable-libatomic --disable-libstdcxx
    make $JOBS all-gcc all-target-libgcc
    make install-gcc install-target-libgcc
}

# The minimum kernel stays glibc's default, 3.2.0, so that glibc keeps its
# fallbacks from the newer calls to the older ones Rach2 has. CXX names a
# compiler that does not exist yet, so configure builds no C++ tests.
step_glibc()
{
    s="$(extract glibc)"
    cd "$(fresh glibc)"
    CC="$TARGET-gcc" CXX="$TARGET-g++" "$s/configure" --prefix=/usr \
        --host="$TARGET" --build="$BUILD" --with-headers="$SYSROOT/usr/include" \
        --enable-kernel=3.2 --disable-werror --disable-nscd --disable-profile
    make $JOBS
    make install install_root="$SYSROOT"
}

step_gcc2()
{
    s="$SRC/gcc-16.2.0"
    [ -d "$s/gmp" ] || s="$(gcc_source)"
    cd "$(fresh gcc2)"
    "$s/configure" $GCC_COMMON --enable-languages=c,c++ --enable-shared \
        --enable-threads=posix --enable-__cxa_atexit --disable-libstdcxx-pch
    make $JOBS
    make install
}

step_libxcrypt()
{
    s="$(extract libxcrypt)"
    cd "$(fresh libxcrypt)"
    "$s/configure" --host="$TARGET" --build="$BUILD" --prefix=/usr \
        --disable-werror --enable-hashes=strong,glibc --enable-obsolete-api=glibc
    make $JOBS
    make install DESTDIR="$SYSROOT"
}

if [ $# -eq 0 ]; then
    rm -rf "$SRC" "$WORK" "$CROSS" "$SYSROOT"
    set -- binutils headers gcc1 glibc gcc2 libxcrypt
fi
for step in "$@"; do
    case "$step" in
        binutils|headers|gcc1|glibc|gcc2|libxcrypt) ;;
        *) echo "gnu: toolchain: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: toolchain: $step"
    t0=$(date +%s)
    ( "step_$step" )
    echo "gnu: toolchain: $step: done in $(($(date +%s) - t0)) s"
done
