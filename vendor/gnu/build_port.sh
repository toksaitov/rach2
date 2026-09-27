#!/usr/bin/env sh

#
# build_port.sh: build programs of the GNU layer with its cross toolchain.
#
# Usage: build_port.sh NAME...
#
# Each NAME has a recipe, ports/NAME.sh, a shell fragment that sets VERSION,
# CONFIGURE_ARGS, and CACHE, the configure answers a cross build cannot find
# out for itself, and may set PATCHES, pinned patches applied after the
# extract with PATCH_STRIP leading directories removed. The program is
# configured for i686-linux-gnu with the compilers `make gnu-toolchain`
# built, compiled, and installed into build/vendor/gnu/root, the tree the
# GNU system is staged from. It is linked statically unless GNU_LINK is
# "shared".
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
ROOT="$OUT/root"
TARGET=i686-linux-gnu
PATH="$OUT/cross/bin:$PATH"
export PATH

case " $MAKEFLAGS " in
    *jobserver*) JOBS= ;;
    *) JOBS="-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" ;;
esac
if [ ! -x "$OUT/cross/bin/$TARGET-gcc" ]; then
    echo "gnu: port: no cross compiler; run make gnu-toolchain first" >&2
    exit 1
fi
BUILD="$(cc -dumpmachine)"
LINK="-static"
if [ "${GNU_LINK:-static}" = shared ]; then
    LINK=""
fi

for name in "$@"; do
    recipe="$DIR/ports/$name.sh"
    if [ ! -f "$recipe" ]; then
        echo "gnu: port: no recipe for $name" >&2
        exit 2
    fi
    VERSION=
    CONFIGURE_ARGS=
    CACHE=
    PATCHES=
    PATCH_STRIP=1
    . "$recipe"

    echo "gnu: port: $name $VERSION"
    t0=$(date +%s)
    tarball="$("$DIR/fetch.sh" --path "$name")"
    src="$OUT/src/$name-$VERSION"
    mkdir -p "$OUT/src"
    rm -rf "$src"
    tar -xf "$tarball" -C "$OUT/src"
    for p in $PATCHES; do
        patch -d "$src" -p"$PATCH_STRIP" -F 0 --forward --silent < "$("$DIR/fetch.sh" --path "$p")"
    done
    work="$OUT/work/port-$name"
    rm -rf "$work"
    mkdir -p "$work"
    (
        cd "$work"
        env $CACHE CC="$OUT/cross/bin/$TARGET-gcc" LDFLAGS="$LINK" "$src/configure" \
            --host="$TARGET" \
            --build="$BUILD" --prefix=/usr $CONFIGURE_ARGS
        make $JOBS
        make install DESTDIR="$ROOT"
    )
    echo "gnu: port: $name: done in $(($(date +%s) - t0)) s"
done
