#!/usr/bin/env sh
# Build the six console keyboard tools against Rach2's static musl.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
VERSION="2.10.0"
TARBALL="$SCRIPT_DIR/kbd-$VERSION.tar.xz"
SHA256="6e5ca4f8d76ee9e3a8db700b667f13e12aac9933828a64e1aaad93d26be9b479"
WORK="$ARCH_BUILD/vendor-work/kbd"
SRC="$WORK/kbd-$VERSION"
STAGE="$ARCH_BUILD/vendor/kbd"
WRAP="$REPO/vendor/musl/muslgcc"
PROGRAMS="loadkeys dumpkeys setleds kbdrate showkey kbd_mode"
GOT=$("$REPO/build/tools/verify/sha256" "$TARBALL" | awk '{print $1}')
if [ "$GOT" != "$SHA256" ]; then
    echo "kbd: sha256 mismatch for $TARBALL" >&2
    exit 1
fi
mkdir -p "$WORK" "$STAGE"
rm -rf "$SRC"
tar -xf "$TARBALL" -C "$WORK"
while IFS= read -r p; do
    case "$p" in ''|'#'*) continue ;; esac
    patch -p1 -F 0 --forward -d "$SRC" < "$SCRIPT_DIR/patches/$p"
done < "$SCRIPT_DIR/patches/series"
for p in "$SCRIPT_DIR"/patches/*.patch; do
    grep -qx "$(basename "$p")" "$SCRIPT_DIR/patches/series" || {
        echo "kbd: patch absent from series: $p" >&2
        exit 1
    }
done
BUILD_TRIPLE=$(sh "$SRC/config/config.guess")
(
    cd "$SRC"
    # Neither a host pkg-config file nor a host compression library is used.
    CC="$WRAP" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip" \
        PKG_CONFIG_LIBDIR="$WORK/pkgconfig" PKG_CONFIG_PATH= \
        ac_cv_lib_bz2_BZ2_bzDecompressInit=no \
        ./configure --build="$BUILD_TRIPLE" --host="$HOST_TRIPLE" \
        --prefix=/usr --datadir=/usr/share/kbd --disable-shared --enable-static \
        --disable-nls --disable-vlock --disable-tests --disable-memcheck \
        --disable-xkb --disable-compress --without-zlib --without-bzip2 \
        --without-lzma --without-zstd LDFLAGS=-static
)
# Generated parser and scanner ship in the release; these commands fail if
# make ever attempts to regenerate either of them.
make -C "$SRC/src/libcommon" all
make -C "$SRC/src/libkbdfile" all
make -C "$SRC/src/libkeymap" all YACC=false LEX=false
make -C "$SRC/src" $PROGRAMS YACC=false LEX=false
for p in $PROGRAMS; do
    "${CROSS}strip" "$SRC/src/$p" -o "$STAGE/$p"
done
printf 'kbd: result: built %s for %s\n' "$VERSION" "$RACH2_ARCH"
