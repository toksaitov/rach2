#!/bin/sh
# Build independent helper tools beside the unchanged GNU compiler and root.
set -eu
DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH='' cd -- "$DIR/../../.." && pwd)
. "$REPO/vendor/gnu/arch.sh"
OUT=$GNU_OUT
WORK=$OUT/work/fstools
DEST=$OUT/fstools
CROSS=$OUT/cross
SYSROOT=$OUT/sysroot
DL=$REPO/vendor/gnu/downloads
SHA=$REPO/build/tools/verify/sha256
PART=

cleanup()
{
    status=$?
    [ -z "$PART" ] || rm -f "$PART"
    if [ "$status" -ne 0 ]; then
        echo "gnu: fstools: build evidence: $WORK" >&2
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The dedicated fixed pin does not change the GNU ports or compiler keys.
# shellcheck disable=SC2046 # a noncomment pin line has five fields
set -- $(awk '$1 !~ /^#/ && NF { print }' "$DIR/sources.txt")
[ "$#" -eq 5 ] && [ "$1" = exfatprogs ] || {
    echo 'gnu: fstools: invalid source pin' >&2
    exit 1
}
VERSION=$2 URL=$3 WANT=$4 SIZE=$5
ARCHIVE=$DL/${URL##*/}
verify()
{
    [ "$(wc -c < "$1")" -eq "$SIZE" ] || return 1
    got=$("$SHA" "$1") || return 1
    [ "${got%% *}" = "$WANT" ]
}

[ -x "$CROSS/bin/$GNU_TARGET-gcc" ] && [ -f "$SYSROOT/usr/include/stdio.h" ]
mkdir -p "$DL"
if [ ! -f "$ARCHIVE" ]; then
    PART=$(mktemp "$DL/.exfatprogs.XXXXXX")
    if command -v curl > /dev/null 2>&1; then
        curl -sSfL --retry 3 -o "$PART" "$URL"
    else
        wget -q -O "$PART" "$URL"
    fi
    verify "$PART" || { echo 'gnu: fstools: refused downloaded source' >&2; exit 1; }
    mv "$PART" "$ARCHIVE"
    PART=
fi
verify "$ARCHIVE" || { echo "gnu: fstools: refused source: $ARCHIVE" >&2; exit 1; }
rm -rf "$WORK" "$DEST"
mkdir -p "$WORK/source" "$WORK/build" "$WORK/install" "$DEST/usr/sbin"
tar -xf "$ARCHIVE" -C "$WORK/source"
SOURCE=$WORK/source/exfatprogs-$VERSION
BUILD=$(cc -dumpmachine)
PATH=$GNU_PATH$CROSS/bin:$PATH
export PATH
# Both positional-write ABIs are wrapped, including direct mkfs FAT writes.
# The object has no dependency on upstream headers or generated files.
"$CROSS/bin/$GNU_TARGET-gcc" --sysroot="$SYSROOT" -std=c99 -O2 -fPIC \
    -Wall -Wextra -Werror -c "$DIR/positional_io.c" -o "$WORK/build/positional_io.o"
cd "$WORK/build"
env LDFLAGS="$WORK/build/positional_io.o -Wl,--wrap=pwrite -Wl,--wrap=pwrite64" \
    CC="$CROSS/bin/$GNU_TARGET-gcc --sysroot=$SYSROOT" \
    AR="$CROSS/bin/$GNU_TARGET-ar" RANLIB="$CROSS/bin/$GNU_TARGET-ranlib" \
    "$SOURCE/configure" --build="$BUILD" --host="$GNU_TARGET" --prefix=/usr --disable-shared
make
make DESTDIR="$WORK/install" install
for name in mkfs.exfat fsck.exfat; do
    test -x "$WORK/install/usr/sbin/$name"
    cp "$WORK/install/usr/sbin/$name" "$DEST/usr/sbin/$name"
done
printf 'etude-fstools 1\nexfatprogs %s\nsha256 %s\nsize %s\ntarget %s\n' \
    "$VERSION" "$WANT" "$SIZE" "$GNU_TARGET" > "$DEST/manifest"
rm -rf "$WORK"
echo "gnu: fstools: prepared exfatprogs $VERSION for $GNU_TARGET"
