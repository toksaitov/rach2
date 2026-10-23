#!/bin/sh
# Build the pinned mtools multicall program for the image helper.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
. "$REPO/mk/arch.sh"
VERSION=4.0.49
TARBALL="$SCRIPT_DIR/mtools-$VERSION.tar.gz"
SHA256=10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76
WORK="$ARCH_VENDOR/mtools"
SRC="$WORK/mtools-$VERSION"
STAGE="$ARCH_BUILD/vendor/mtools"

got=$("$REPO/build/tools/verify/sha256" "$TARBALL")
test "${got%% *}" = "$SHA256" || {
    echo 'mtools: the source tarball has another digest' >&2
    exit 1
}
test -f "$MUSL_PREFIX/lib/libc.a"
rm -rf "$SRC"
mkdir -p "$WORK" "$STAGE"
tar xzf "$TARBALL" -C "$WORK"
cd "$SRC"
./configure --host="$HOST_TRIPLE" --without-x --disable-floppyd \
    CC="$REPO/vendor/musl/muslgcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
    CFLAGS=-O2 > configure.log 2>&1 || { tail -20 configure.log >&2; exit 1; }
case " ${MAKEFLAGS-} " in
    *' --jobserver-auth='*|*' --jobserver-fds='*) JOBFLAG= ;;
    *) JOBFLAG="-j${JOBS:-4}" ;;
esac
make ${JOBFLAG:+"$JOBFLAG"} mtools > build.log 2>&1 || {
    tail -20 build.log >&2
    exit 1
}
"${CROSS}strip" -o "$STAGE/mtools" mtools
