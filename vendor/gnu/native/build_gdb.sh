#!/usr/bin/env sh

#
# build_gdb.sh: GDB and gdbserver built to run on Rach2.
#
# Usage: build_gdb.sh [STEP...]
#
# Builds GDB and its gdbserver, at the version sources.txt pins, on this Linux
# host with the cross toolchain `make gnu-toolchain` built, as programs that run
# on the toolchain's target, i686-linux-gnu or x86_64-rach2-linux-gnu as
# RACH2_ARCH names the architecture (vendor/gnu/arch.sh), and debug programs
# there. They link dynamically against the toolchain's glibc and install under
# the prefix /usr into gdb/ of the layer's build tree, the staging root. GDB
# needs GMP and MPFR, expat to read its XML files, the tables of system calls by
# name among them, and ncurses for its text user interface; they are built first
# as static libraries that GDB links, so that the programs need nothing on Rach2
# beyond glibc, the C++ runtime and the terminfo database. The steps are gmp,
# mpfr, expat, ncurses, gdb and check; with no STEP all six run from nothing,
# and naming steps runs only those over what an earlier run left. Each step
# writes its output to a log in log/ of the build tree. Under make the sub-makes
# share the caller's jobs; run alone, the script uses GNU_JOBS processors, or
# every processor when GNU_JOBS is not set.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
OUT="$GNU_OUT"
SRC="$OUT/src/gdb"
WORK="$OUT/work"
LOG="$OUT/log"
CROSS="$OUT/cross"
SYSROOT="$OUT/sysroot"
DEPS="$WORK/gdb-deps"
ROOT="$OUT/gdb"
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
    echo "gnu: gdb: no cross compiler; run make gnu-toolchain first" >&2
    exit 1
fi
for tool in cc c++ make m4 makeinfo xz unshare; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: gdb: $tool not found; the GNU layer builds on a Linux host with the tools vendor/gnu/Readme.md lists" >&2
        exit 1
    fi
done
BUILD="$(cc -dumpmachine)"

# extract NAME: unpack a checked tarball afresh and print its tree.
extract()
{
    tarball="$("$GNU/fetch.sh" --path "$1")"
    tree="$SRC/$(basename "$tarball" | sed -e 's/\.tar\..*$//')"
    mkdir -p "$SRC"
    rm -rf "$tree"
    tar -xf "$tarball" -C "$SRC"
    echo "$tree"
}

# fresh NAME: an empty build directory for one step.
fresh()
{
    rm -rf "${WORK:?}/gdb-$1"
    mkdir -p "$WORK/gdb-$1"
    echo "$WORK/gdb-$1"
}

# GMP's configure tests the compiler with C written before prototypes, which
# GCC 16 rejects in its default C23, where f() declares a function of no
# arguments, so the compiler is given the C17 of GNU.
step_gmp()
{
    s="$(extract gmp)"
    cd "$(fresh gmp)"
    CC="$TARGET-gcc -std=gnu17" "$s/configure" --build="$BUILD" --host="$TARGET" \
        --prefix="$DEPS" --disable-shared --enable-static
    make ${JOBS:+"$JOBS"}
    make install
}

step_mpfr()
{
    s="$(extract mpfr)"
    cd "$(fresh mpfr)"
    "$s/configure" --build="$BUILD" --host="$TARGET" --prefix="$DEPS" \
        --with-gmp="$DEPS" --disable-shared --enable-static
    make ${JOBS:+"$JOBS"}
    make install
}

step_expat()
{
    s="$(extract expat)"
    cd "$(fresh expat)"
    "$s/configure" --build="$BUILD" --host="$TARGET" --prefix="$DEPS" \
        --disable-shared --enable-static --without-docbook --without-examples \
        --without-tests
    make ${JOBS:+"$JOBS"}
    make install
}

# ncurses is built with wide characters, its default, and without its
# programs, its database and its C++ binding. The library looks for terminal
# descriptions in /usr/share/terminfo, where the root's ncurses port installs
# them.
step_ncurses()
{
    s="$(extract ncurses)"
    cd "$(fresh ncurses)"
    "$s/configure" --build="$BUILD" --host="$TARGET" --prefix="$DEPS" \
        --without-shared --without-debug --without-ada --without-cxx-binding \
        --without-progs --without-tests --without-manpages --disable-db-install \
        --with-default-terminfo-dir=/usr/share/terminfo \
        --with-terminfo-dirs=/usr/share/terminfo
    make ${JOBS:+"$JOBS"}
    make install
}

# The top-level configure looks for GMP and MPFR with --with-gmp and
# --with-mpfr, and gdb's own with the --with-lib*-prefix options, expat's
# among them, which `catch syscall write` needs to find the call by its name.
# gdb's configure finds ncurses by its header and library alone, so the
# dependencies' directories are given to the compiler and the linker, and
# --enable-tui makes a missing ncurses an error. The other optional libraries
# GDB would look for (Python, Guile, the compressors, debuginfod,
# source-highlight) are left out. The tarball has the bfd and opcodes of
# GDB's own release, so only gdb and gdbserver are installed.
step_gdb()
{
    s="$(extract gdb)"
    cd "$(fresh gdb)"
    CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" \
        "$s/configure" --build="$BUILD" --host="$TARGET" --target="$TARGET" \
        --prefix=/usr --disable-nls --disable-werror --disable-sim \
        --with-gmp="$DEPS" --with-mpfr="$DEPS" \
        --with-libgmp-prefix="$DEPS" --with-libmpfr-prefix="$DEPS" \
        --with-expat --with-libexpat-prefix="$DEPS" --enable-tui \
        --without-python --without-guile --without-lzma \
        --without-zstd --without-xxhash --without-debuginfod \
        --without-babeltrace --without-intel-pt --disable-source-highlight
    make ${JOBS:+"$JOBS"} all-gdb all-gdbserver
    make install-strip-gdb install-strip-gdbserver DESTDIR="$ROOT"
}

# The check starts both programs through the sysroot's loader, with the cross
# toolchain's C++ runtime, prints their versions, and requires GDB's
# configuration to have the text user interface. Then, in a root that
# has the sysroot, the staging root and that runtime, GDB debugs a program:
# unshare(1) makes the user root of new user, mount and process namespaces,
# chroots, and mounts a /proc, which GDB reads for the threads of the program
# it runs. The program's debugging information has its source's path as it
# is in the root, and the safe path lets GDB load glibc's libthread_db from
# /lib. The check requires the stop at the breakpoint, the backtrace, the
# value printed, and the program's output and normal exit.
step_check()
{
    lib="$SYSROOT/lib:$SYSROOT/usr/lib:$CROSS/$TARGET/lib"
    v="$("$SYSROOT$GNU_LOADER" --library-path "$lib" "$ROOT/usr/bin/gdb" --version)"
    echo "$v" | sed -n 1p
    "$SYSROOT$GNU_LOADER" --library-path "$lib" "$ROOT/usr/bin/gdb" --configuration |
        grep -e --enable-tui
    v="$("$SYSROOT$GNU_LOADER" --library-path "$lib" "$ROOT/usr/bin/gdbserver" --version)"
    echo "$v" | sed -n 1p
    r="$(fresh check)/root"
    mkdir -p "$r/tmp" "$r/proc" "$r/check" "$r/usr/lib"
    cp -a "$SYSROOT/." "$r/"
    cp -a "$ROOT/." "$r/"
    cp -a "$CROSS/$TARGET/lib/libstdc++.so"* "$CROSS/$TARGET/lib/libgcc_s.so"* \
        "$CROSS/$TARGET/lib/libatomic.so"* "$r/usr/lib/"
    "$TARGET-readelf" -d "$ROOT/usr/bin/gdb" | grep NEEDED
    cat > "$r/check/count.c" <<'EOF'
#include <stdio.h>

static int square(int n)
{
    int s = n * n;
    return s;
}

int main(void)
{
    int total = 0;
    int i;

    for (i = 1; i <= 3; i++)
        total += square(i);
    printf("total %d\n", total);
    return 0;
}
EOF
    "$TARGET-gcc" -g -O0 -fdebug-prefix-map="$r/"=/ -o "$r/check/count" "$r/check/count.c"
    HOME=/tmp unshare --map-root-user --mount --pid --fork --mount-proc=/proc --root="$r" \
        /usr/bin/gdb -nx -batch -iex 'set auto-load safe-path /' \
        -ex 'set startup-with-shell off' -ex 'break square' -ex run \
        -ex backtrace -ex next -ex 'print s' -ex delete -ex continue /check/count \
        > "$r/check/gdb.out" 2>&1
    cat "$r/check/gdb.out"
    grep -q '^Breakpoint 1, square (n=1)' "$r/check/gdb.out"
    grep -q '^#1 .* in main () at /check/count.c:15' "$r/check/gdb.out"
    grep -q -x -F -e "\$1 = 1" "$r/check/gdb.out"
    grep -q '^total 14$' "$r/check/gdb.out"
    grep -q 'exited normally' "$r/check/gdb.out"
}

if [ $# -eq 0 ]; then
    rm -rf "$SRC" "$WORK"/gdb-* "$ROOT"
    set -- gmp mpfr expat ncurses gdb check
fi
mkdir -p "$LOG"
for step in "$@"; do
    case "$step" in
        gmp|mpfr|expat|ncurses|gdb|check) ;;
        *) echo "gnu: gdb: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: gdb: $step"
    log="$LOG/gdb-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: gdb: $step: failed; the log is $log" >&2
        exit 1
    fi
    if [ "$step" = check ]; then
        cat "$log"
    fi
    echo "gnu: gdb: $step: done in $(($(date +%s) - t0)) s"
done
