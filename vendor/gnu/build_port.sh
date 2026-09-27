#!/usr/bin/env sh

#
# build_port.sh: build programs of the GNU layer with its cross toolchain.
#
# Usage: build_port.sh NAME...
#        build_port.sh --all
#
# Each NAME has a recipe, ports/NAME.sh, a shell fragment this script reads.
# It sets VERSION; DEPENDS, the ports whose headers and libraries it needs;
# CONFIGURE_ARGS; CACHE, the configure answers a cross build cannot find out
# for itself; and PATCHES, pinned patches applied after the extract with
# PATCH_STRIP leading directories removed. By default a port is configured
# by autoconf in a directory of its own, compiled, and installed. A port
# built another way defines port_build, which runs in that directory, and
# port_install, which installs into $DEST. SOURCE is the name of the port's
# tarball in sources.txt when it is not NAME, and empty for a port with none.
#
# The compilers run with the root, build/vendor/gnu/root, as their sysroot,
# and pkg-config reads only the root's files, so a port finds what the ports
# before it installed where the target system has it. A port installs into
# stage/NAME, where the debugging information is stripped from its ELF files,
# which keep their symbol tables, and its libtool archives and Info index are
# removed; the rest is copied into the root and listed in stage/NAME.files. A
# port is built for another's DEPENDS only when it has no such list, and
# every port depends on the runtime, the toolchain's glibc. --all builds
# every recipe into an empty root. Programs link with the shared libraries;
# GNU_LINK=static links the ports configured by autoconf statically instead,
# and gives make -all-static in a port with libtool, which reads -static as
# "no uninstalled shared libraries" and links the C library shared.
# Under make, the makes a port runs share the caller's jobs; run alone, the
# script gives each as many jobs as there are processors.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
CROSS="$OUT/cross"
# shellcheck disable=SC2034 # the runtime's recipe reads it
SYSROOT="$OUT/sysroot"
ROOT="$OUT/root"
STAGE="$OUT/stage"
WRAP="$OUT/wrap"
TARGET=i686-linux-gnu

case " $MAKEFLAGS " in
    *jobserver*) ;;
    *)
        MAKEFLAGS="$MAKEFLAGS -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
        export MAKEFLAGS
        ;;
esac
if [ ! -x "$CROSS/bin/$TARGET-gcc" ]; then
    echo "gnu: port: no cross compiler; run make gnu-toolchain first" >&2
    exit 1
fi
BUILD="$(cc -dumpmachine)"
GNU_LINK="${GNU_LINK:-shared}"
case "$GNU_LINK" in
    shared) LINK= ;;
    static) LINK=-static ;;
    *)
        echo "gnu: port: GNU_LINK is shared or static, not $GNU_LINK" >&2
        exit 2
        ;;
esac

mkdir -p "$WRAP"
for tool in gcc g++ c++ cpp; do
    # shellcheck disable=SC2016 # "$@" belongs to the wrapper
    printf '#!/bin/sh\nexec "%s" --sysroot="%s" "$@"\n' \
        "$CROSS/bin/$TARGET-$tool" "$ROOT" >"$WRAP/$TARGET-$tool"
    chmod +x "$WRAP/$TARGET-$tool"
done
PATH="$WRAP:$CROSS/bin:$PATH"
PKG_CONFIG_LIBDIR="$ROOT/usr/lib/pkgconfig:$ROOT/usr/share/pkgconfig"
PKG_CONFIG_SYSROOT_DIR="$ROOT"
PKG_CONFIG_PATH=
export PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_PATH

# configure_autoconf ARG...: run the port's configure script for the target,
# in the current directory, with the recipe's arguments and then these.
# shellcheck disable=SC2120 # the recipes pass arguments
configure_autoconf()
{
    # shellcheck disable=SC2086 # CACHE and CONFIGURE_ARGS are lists of words
    env $CACHE CC="$TARGET-gcc" CXX="$TARGET-g++" LDFLAGS="$LINK" \
        "$SRC/configure" --host="$TARGET" --build="$BUILD" --prefix=/usr \
        $CONFIGURE_ARGS "$@"
}

# libtool_static: the make argument a static link needs in a port whose
# configure made a libtool script; nothing for a shared link or a port
# without one.
libtool_static()
{
    if [ "$GNU_LINK" = static ] && [ -f libtool ]; then
        echo LDFLAGS=-all-static
    fi
}

# depends NAME: print the ports NAME needs before it, one a line.
depends()
{
    [ "$1" = runtime ] || echo runtime
    (
        DEPENDS=
        # shellcheck source=/dev/null
        . "$DIR/ports/$1.sh"
        for d in $DEPENDS; do
            echo "$d"
        done
    )
}

# order NAME...: print the names, each after the ports it needs; a name may
# repeat, and its first place is the one that counts.
order()
{
    for n in "$@"; do
        # shellcheck disable=SC2046 # a port's name is one word
        (order $(depends "$n"))
        echo "$n"
    done
}

# build NAME: build one port, install it into its stage, and copy that into
# the root.
build()
{
    name=$1
    if [ ! -f "$DIR/ports/$name.sh" ]; then
        echo "gnu: port: no recipe for $name" >&2
        exit 2
    fi
    VERSION=
    CONFIGURE_ARGS=
    CACHE=
    PATCHES=
    PATCH_STRIP=1
    SOURCE=$name
    port_build()
    {
        configure_autoconf
        # shellcheck disable=SC2046 # one word or none
        make $(libtool_static)
    }
    port_install()
    {
        make install DESTDIR="$DEST"
    }
    # shellcheck source=/dev/null
    . "$DIR/ports/$name.sh"

    echo "gnu: port: $name $VERSION"
    t0=$(date +%s)
    SRC=
    if [ -n "$SOURCE" ]; then
        tarball="$("$DIR/fetch.sh" --path "$SOURCE")"
        SRC="$OUT/src/$SOURCE-$VERSION"
        mkdir -p "$OUT/src"
        rm -rf "$SRC"
        tar -xf "$tarball" -C "$OUT/src"
        for p in $PATCHES; do
            patch -d "$SRC" -p"$PATCH_STRIP" -F 0 --forward --silent \
                <"$("$DIR/fetch.sh" --path "$p")"
        done
    fi
    WORK="$OUT/work/port-$name"
    DEST="$STAGE/$name"
    rm -rf "$WORK" "$DEST" "$STAGE/$name.files"
    mkdir -p "$WORK" "$DEST" "$ROOT"
    (
        cd "$WORK"
        port_build
    )
    (
        cd "$WORK"
        port_install
    )
    find "$DEST" -type f \( -perm -u+x -o -name '*.so*' -o -name '*.[ao]' \) \
        -exec "$TARGET-strip" --strip-debug {} + 2>/dev/null || true
    find "$DEST" -type f -name '*.la' -exec rm -f {} +
    rm -f "$DEST/usr/share/info/dir"
    cp -af "$DEST/." "$ROOT/"
    (
        cd "$DEST"
        find . ! -type d | LC_ALL=C sort
    ) >"$STAGE/$name.files"
    size=$(du -sk "$DEST" | awk '{ printf "%.1f", $1 / 1024 }')
    echo "gnu: port: $name: done in $(($(date +%s) - t0)) s, $size MB installed"
}

if [ "$1" = --all ]; then
    rm -rf "$ROOT" "$STAGE"
    all=
    for f in "$DIR"/ports/*.sh; do
        all="$all $(basename "$f" .sh)"
    done
    # shellcheck disable=SC2086 # a port's name is one word
    set -- $all
fi
for name in "$@"; do
    if [ ! -f "$DIR/ports/$name.sh" ]; then
        echo "gnu: port: no recipe for $name" >&2
        exit 2
    fi
done
for name in $(order "$@" | awk '!seen[$0]++'); do
    case " $* " in
        *" $name "*)
            build "$name"
            ;;
        *)
            if [ ! -f "$STAGE/$name.files" ]; then
                build "$name"
            fi
            ;;
    esac
done
