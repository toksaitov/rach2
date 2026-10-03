#!/usr/bin/env sh

#
# build_native.sh: binutils and GCC built to run on Rach2.
#
# Usage: build_native.sh [STEP...]
#
# Builds binutils 2.47 and GCC 16.2.0 for C and C++ on this Linux host, with
# the cross toolchain `make gnu-toolchain` built, as programs that run on the
# toolchain's target, i686-linux-gnu or x86_64-rach2-linux-gnu as RACH2_ARCH
# names the architecture (vendor/gnu/arch.sh), and make code for it: a native
# compiler for Rach2 made by a cross build. They link dynamically against the
# toolchain's glibc and install under the prefix /usr into native/ of the
# layer's build tree, the staging root. The steps are binutils, gcc and check;
# with no STEP all three run from nothing, and naming steps runs only those
# over what an earlier run left. Each step writes its output to a log in log/
# of the build tree. Under make the sub-makes share the caller's jobs; run
# alone, the script uses GNU_JOBS processors, or every processor when GNU_JOBS
# is not set.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
OUT="$GNU_OUT"
SRC="$OUT/src/native"
WORK="$OUT/work"
LOG="$OUT/log"
CROSS="$OUT/cross"
SYSROOT="$OUT/sysroot"
ROOT="$OUT/native"
TARGET="$GNU_TARGET"
PATH="$CROSS/bin:$PATH"
PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
unset PKG_CONFIG_PATH

case " $MAKEFLAGS " in
    *jobserver*) JOBS= ;;
    *) JOBS="-j${GNU_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}" ;;
esac

if [ ! -x "$CROSS/bin/$TARGET-gcc" ]; then
    echo "gnu: native: no cross compiler; run make gnu-toolchain first" >&2
    exit 1
fi
for tool in cc c++ make gawk bison flex m4 makeinfo xz unshare; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: native: $tool not found; the GNU layer builds on a Linux host with the tools vendor/gnu/Readme.md lists" >&2
        exit 1
    fi
done
BUILD="$(cc -dumpmachine)"

# extract NAME [DEST]: unpack a checked tarball afresh and print its tree.
extract()
{
    tarball="$("$GNU/fetch.sh" --path "$1")"
    dest="${2:-$SRC}"
    tree="$dest/$(basename "$tarball" | sed -e 's/\.tar\..*$//' -e 's/\.tgz$//')"
    mkdir -p "$dest"
    rm -rf "$tree"
    tar -xf "$tarball" -C "$dest"
    echo "$tree"
}

# fresh NAME: an empty build directory for one step.
fresh()
{
    rm -rf "${WORK:?}/native-$1"
    mkdir -p "$WORK/native-$1"
    echo "$WORK/native-$1"
}

# pkg-config is pointed at the sysroot, so that no library of this host is
# found and linked. --with-sysroot=/ lets ld take --sysroot, as a native
# linker configured with one does.
step_binutils()
{
    s="$(extract binutils)"
    cd "$(fresh binutils)"
    "$s/configure" --build="$BUILD" --host="$TARGET" --target="$TARGET" \
        --prefix=/usr --with-sysroot=/ --disable-nls --disable-werror \
        --disable-gprofng --without-zstd --without-debuginfod --without-msgpack
    make ${JOBS:+"$JOBS"}
    make install-strip DESTDIR="$ROOT"
}

# The build machine differs from the host and the target, which are the same:
# a crossed native build, in the words of GCC's manual. The cross compiler
# compiles the host programs, and the target libraries as well, against the
# sysroot given by --with-build-sysroot. The installed compiler has no
# sysroot and looks in /usr, as a native one does. GMP, MPFR and MPC are
# built in GCC's tree, as for the cross compiler, and the target libraries
# are the ones it has. As build_toolchain.sh does for the cross compiler,
# the x86-64 compiler is told its libraries are in lib, not lib64.
step_gcc()
{
    s="$(extract gcc)"
    for lib in gmp mpfr mpc; do
        t="$(extract "$lib" "$SRC/$lib-tree")"
        mv "$t" "$s/$lib"
    done
    rm -rf "$SRC/gmp-tree" "$SRC/mpfr-tree" "$SRC/mpc-tree"
    sed -i '/^MULTILIB_OSDIRNAMES = m64=/s|m64=../lib64|m64=../lib|' "$s/gcc/config/i386/t-linux64"
    # shellcheck disable=SC2016 # the pattern's $( is literal
    if ! grep -q '^MULTILIB_OSDIRNAMES = m64=../lib[$(]' "$s/gcc/config/i386/t-linux64"; then
        echo "gnu: native: gcc/config/i386/t-linux64 has no m64 line to point at lib" >&2
        exit 1
    fi
    cd "$(fresh gcc)"
    "$s/configure" --build="$BUILD" --host="$TARGET" --target="$TARGET" \
        --prefix=/usr --with-build-sysroot="$SYSROOT" \
        --enable-languages=c,c++ --with-arch="$GNU_GCC_ARCH" --with-tune=generic \
        --disable-multilib --disable-nls --disable-bootstrap \
        --enable-shared --enable-threads=posix --enable-__cxa_atexit \
        --disable-libstdcxx-pch --disable-libitm --disable-libquadmath \
        --disable-libssp --disable-libvtv
    make ${JOBS:+"$JOBS"}
    make install-strip DESTDIR="$ROOT"
}

# The check copies the sysroot and the staging root into one tree, the
# system a GNU program on Rach2 will see, and runs the new compiler there
# under unshare(1): a user namespace makes the user root, so the chroot needs
# no privilege, and the loader in the tree is the sysroot's.
step_check()
{
    r="$(fresh check)/root"
    mkdir -p "$r/tmp" "$r/check"
    cp -a "$SYSROOT/." "$r/"
    cp -a "$ROOT/." "$r/"
    cat > "$r/check/hello.c" <<'EOF'
#include <stdio.h>

int main(void)
{
    printf("hello from C\n");
    return 0;
}
EOF
    cat > "$r/check/hello.cc" <<'EOF'
#include <iostream>
#include <string>

int main()
{
    std::string who = "C++";
    std::cout << "hello from " << who << std::endl;
    return 0;
}
EOF
    "$SYSROOT$GNU_LOADER" --library-path "$SYSROOT/lib:$ROOT/usr/lib" \
        "$ROOT/usr/bin/gcc" --version | sed -n 1p
    unshare --map-root-user --root="$r" /usr/bin/gcc -O2 -o /check/hello-c /check/hello.c
    unshare --map-root-user --root="$r" /usr/bin/g++ -O2 -o /check/hello-cxx /check/hello.cc
    c="$(unshare --map-root-user --root="$r" /check/hello-c)"
    cxx="$(unshare --map-root-user --root="$r" /check/hello-cxx)"
    echo "$c"
    echo "$cxx"
    "$TARGET-readelf" -l "$r/check/hello-cxx" | grep 'program interpreter'
    "$TARGET-readelf" -d "$r/check/hello-cxx" | grep NEEDED
    [ "$c" = "hello from C" ] && [ "$cxx" = "hello from C++" ]
}

if [ $# -eq 0 ]; then
    rm -rf "$SRC" "$WORK"/native-* "$ROOT"
    set -- binutils gcc check
fi
mkdir -p "$LOG"
for step in "$@"; do
    case "$step" in
        binutils|gcc|check) ;;
        *) echo "gnu: native: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: native: $step"
    log="$LOG/native-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: native: $step: failed; the log is $log" >&2
        exit 1
    fi
    if [ "$step" = check ]; then
        cat "$log"
    fi
    echo "gnu: native: $step: done in $(($(date +%s) - t0)) s"
done
