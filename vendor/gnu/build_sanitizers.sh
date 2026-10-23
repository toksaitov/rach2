#!/bin/sh
# Build the pinned GCC's sanitizer runtimes beside the existing toolchain.
# The result is a compiler overlay and a GNU root overlay; no probe runs.
set -eu

DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH='' cd -- "$DIR/../.." && pwd)
. "$DIR/arch.sh"
OUT=$GNU_OUT
DEST=$OUT/sanitizers
WORK=$OUT/work/sanitizers
CROSS=$OUT/cross
SYSROOT=$OUT/sysroot
CC=$CROSS/bin/$GNU_TARGET-gcc
CXX=$CROSS/bin/$GNU_TARGET-g++
[ -x "$CC" ] && [ -x "$CXX" ] || {
    echo 'gnu: sanitizers: prepare the GNU cross toolchain first' >&2
    exit 1
}
case $(uname -s) in
    Linux) ;;
    *) echo 'gnu: sanitizers: preparation needs the GNU build environment' >&2; exit 1 ;;
esac

VERSION=$(awk '$1 == "gcc" { print $2; exit }' "$DIR/sources.txt")
ARCHIVE=$(sh "$DIR/fetch.sh" --path gcc)
PREFIX=gcc-$VERSION
rm -rf "$WORK" "$DEST"
trap 'status=$?; if [ "$status" -ne 0 ]; then
    echo "gnu: sanitizers: build evidence: $WORK" >&2
fi' 0
mkdir -p "$WORK/source" "$WORK/build" "$WORK/install" "$DEST/compiler" "$DEST/root/usr/lib"
# Keep the standalone configure's sibling sources and auxiliary files,
# without unpacking the compiler, its tests or its other runtime libraries.
tar -tf "$ARCHIVE" | awk -v p="$PREFIX/" '
    index($0, p) == 1 {
        n = substr($0, length(p) + 1)
        if (n ~ /^(libsanitizer|libbacktrace|include|config)\// ||
            n == "gcc/BASE-VER" || n ~ /^libiberty\/cp-demangle\.(c|h)$/ ||
            n ~ /^(libtool|ltoptions|ltsugar|ltversion|lt~obsolete)\.m4$/ ||
            n ~ /^(config-ml.in|config.guess|config.sub|install-sh|mkinstalldirs|missing|compile|depcomp|ar-lib|ltmain.sh|multilib.am)$/)
            print
    }' > "$WORK/source.list"
# The list contains directory entries as well as their children. Extract
# each member once; recursive matching consumes the later explicit entries.
tar --no-recursion --verbatim-files-from -xf "$ARCHIVE" \
    -C "$WORK/source" -T "$WORK/source.list"
SOURCE=$WORK/source/$PREFIX
for input in libsanitizer/configure libsanitizer/Makefile.in gcc/BASE-VER \
    libbacktrace/filetype.awk libiberty/cp-demangle.c libiberty/cp-demangle.h \
    config-ml.in config.guess config.sub install-sh multilib.am \
    libtool.m4 ltoptions.m4 ltsugar.m4 ltversion.m4 lt~obsolete.m4; do
    [ -f "$SOURCE/$input" ] || {
        echo "gnu: sanitizers: the pinned archive lacks $input" >&2
        exit 1
    }
done
# Standalone configure assumes an unbuilt sibling libstdc++ tree. Use the
# installed cross compiler's C++ headers and library in every generated
# Makefile: upstream clears MAKEOVERRIDES, losing command-line replacements
# when make recurses from the top directory or into its install rules.
for template in "$SOURCE/libsanitizer/Makefile.in" "$SOURCE/libsanitizer/"*/Makefile.in; do
    sed -e 's/^LIBSTDCXX_RAW_CXX_CXXFLAGS =.*/LIBSTDCXX_RAW_CXX_CXXFLAGS =/' \
        -e 's/^LIBSTDCXX_RAW_CXX_LDFLAGS =.*/LIBSTDCXX_RAW_CXX_LDFLAGS = -lstdc++/' \
        "$template" > "$template.tmp"
    mv "$template.tmp" "$template"
done
# BUILD describes programs run by configure; HOST is the runtime's target.
BUILD=$(cc -dumpmachine)
cd "$WORK/build"
env CC="$CC --sysroot=$SYSROOT" CXX="$CXX --sysroot=$SYSROOT" \
    AR="$CROSS/bin/$GNU_TARGET-ar" RANLIB="$CROSS/bin/$GNU_TARGET-ranlib" \
    AS="$CC" LD="$CROSS/bin/$GNU_TARGET-ld" NM="$CROSS/bin/$GNU_TARGET-nm" \
    "$SOURCE/libsanitizer/configure" --build="$BUILD" --host="$GNU_TARGET" \
        --target="$GNU_TARGET" --prefix=/usr --libdir=/usr/lib \
        --with-toolexeclibdir=/usr/lib --disable-multilib --enable-shared --enable-static \
        --disable-werror
SUBDIRS='sanitizer_common interception libbacktrace'
if [ "$RACH2_ARCH" = x86_64 ]; then
    SUBDIRS="$SUBDIRS lsan"
fi
SUBDIRS="$SUBDIRS asan ubsan"
make SUBDIRS="$SUBDIRS"
make SUBDIRS="$SUBDIRS" DESTDIR="$WORK/install" install

LIB=$WORK/install/usr/lib
[ -f "$LIB/libasan_preinit.o" ] && [ -f "$LIB/libsanitizer.spec" ] && \
    [ -e "$LIB/libasan.so" ] && [ -e "$LIB/libubsan.so" ] || {
    echo 'gnu: sanitizers: the standalone build left incomplete compiler inputs' >&2
    exit 1
}
for file in "$LIB"/libasan* "$LIB"/libubsan* "$LIB"/liblsan* "$LIB/libsanitizer.spec"; do
    [ -e "$file" ] || continue
    cp -a "$file" "$DEST/compiler/"
done
HEADERS=$LIB/gcc/$GNU_TARGET/$VERSION/include/sanitizer
[ -f "$HEADERS/asan_interface.h" ] || {
    echo 'gnu: sanitizers: the build left no sanitizer interface headers' >&2
    exit 1
}
mkdir -p "$DEST/compiler/include"
cp -a "$HEADERS" "$DEST/compiler/include/"
for file in "$LIB"/libasan.so* "$LIB"/libubsan.so* "$LIB"/liblsan.so*; do
    [ -e "$file" ] || continue
    cp -a "$file" "$DEST/root/usr/lib/"
done
# These are the runtime libraries' C++ and unwinder dependencies. The GNU
# root already supplies glibc, libm, libdl and libpthread.
for name in libstdc++.so.6 libgcc_s.so.1; do
    file=$($CXX -print-file-name="$name")
    [ "$file" != "$name" ] && [ -f "$file" ] || {
        echo "gnu: sanitizers: the cross compiler cannot find $name" >&2
        exit 1
    }
    cp -L "$file" "$DEST/root/usr/lib/$name"
done
if [ "$RACH2_ARCH" = x86_64 ] && [ ! -e "$DEST/root/usr/lib/liblsan.so" ]; then
    echo 'gnu: sanitizers: the x86-64 runtime lacks LeakSanitizer' >&2
    exit 1
fi
printf 'etude-sanitizers 1\ngcc %s\ntarget %s\n' "$VERSION" "$GNU_TARGET" > "$DEST/manifest"
# A cached output contains installed files, not its temporary compiler tree.
rm -rf "$WORK"
echo "gnu: sanitizers: prepared GCC $VERSION runtimes for $GNU_TARGET"
