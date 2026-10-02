#!/usr/bin/env sh

#
# build_e2fsprogs.sh: build a static-musl e2fsck, resize2fs, mke2fs, tune2fs
# and dumpe2fs for Rach2 from the pinned, vendored e2fsprogs tarball.
# Verifies the sha256, extracts, applies the patches patches/series names,
# configures against the ported musl through the muslgcc wrapper with every
# program and library this system does not use switched off, builds the
# libraries and the five programs, strips them, and stages them out of tree
# under build/vendor/e2fsprogs/, from which the manifests stage them. mke2fs
# is built with mke2fs.conf beside this script as its defaults. debugfs is
# not built. Run vendor/musl/build_libc.sh first.
#
# Usage: ./build_e2fsprogs.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="1.47.4"
TARBALL="e2fsprogs-${VERSION}.tar.gz"
SRC="e2fsprogs-${VERSION}"
PREFIX="$SCRIPT_DIR/../musl/install"
WRAP="$SCRIPT_DIR/muslgcc"
STAGE="$(cd "$SCRIPT_DIR/../.." && pwd)/build/vendor/e2fsprogs"

# Pinned sha256 of the vendored tarball, from the sha256sums file published
# beside it under kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4.
TARBALL_SHA256="da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96"

sha256_of()
{
    if [ ! -x "$SCRIPT_DIR/../../build/tools/verify/sha256" ]; then
        mkdir -p "$SCRIPT_DIR/../../build/tools/verify"
        cc -O2 -o "$SCRIPT_DIR/../../build/tools/verify/sha256" \
            "$SCRIPT_DIR/../../tools/verify/sha256.c" || return 1
    fi
    "$SCRIPT_DIR/../../build/tools/verify/sha256" "$1" | awk '{print $1}'
}

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "e2fsprogs: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "e2fsprogs: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "e2fsprogs: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "e2fsprogs: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "e2fsprogs: extracting"
rm -rf "$SRC"
tar xzf "$TARBALL"

# The patches patches/series names, in its order. A named patch that is
# absent fails, and so does a patches/*.patch file the series does not name.
if [ -f patches/series ]; then
    while IFS= read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        if [ ! -f "patches/$p" ]; then
            echo "e2fsprogs: patches/series names a missing patch: $p" >&2
            exit 1
        fi
        echo "e2fsprogs: applying $p"
        patch -d "$SRC" -p1 < "patches/$p"
    done < patches/series
    for f in patches/*.patch; do
        [ -e "$f" ] || continue
        if ! grep -qx "$(basename "$f")" patches/series; then
            echo "e2fsprogs: $f is not named in patches/series" >&2
            exit 1
        fi
    done
fi

# mke2fs's defaults are compiled from misc/mke2fs.conf.custom.in when that
# file exists, and are then a volume this kernel mounts writable.
cp mke2fs.conf "$SRC/misc/mke2fs.conf.custom.in"

cd "$SRC"

# The configure run: the cross triplet so that no host program is run as a
# guest one, the wrapper as the compiler, the private uuid and blkid
# libraries e2fsck links, the resizer, and off everything else: message
# catalogs, thread locals, the uuid daemon, fuse2fs, the initrd helper,
# e2image, e4defrag, debugfs, multi-mount protection, backtraces and tdb,
# none of which the five programs need and each of which is a build of its
# own.
echo "e2fsprogs: configuring"
./configure --host=i686-linux-musl CC="$WRAP" AR=i686-elf-ar RANLIB=i686-elf-ranlib \
    --disable-nls --disable-tls --disable-uuidd --disable-fuse2fs \
    --disable-e2initrd-helper --disable-imager --enable-resizer --disable-defrag \
    --disable-debugfs --disable-mmp --disable-backtrace --disable-tdb --disable-rpath \
    --without-libiconv --enable-libuuid --enable-libblkid CFLAGS="-O2" \
    > configure.log 2>&1 || { tail -20 configure.log >&2; exit 1; }

# The libraries first, then e2fsck, resize2fs, mke2fs, tune2fs and dumpe2fs
# alone. RDYNAMIC is the link flag that exports every symbol for a backtrace,
# which is disabled above and which the freestanding linker does not take.
echo "e2fsprogs: building the libraries"
make -j"${JOBS:-4}" libs > build-libs.log 2>&1 || { tail -20 build-libs.log >&2; exit 1; }
echo "e2fsprogs: building e2fsck"
make -j"${JOBS:-4}" -C e2fsck e2fsck RDYNAMIC= > build-e2fsck.log 2>&1 || { tail -20 build-e2fsck.log >&2; exit 1; }
echo "e2fsprogs: building resize2fs"
make -j"${JOBS:-4}" -C resize resize2fs RDYNAMIC= > build-resize2fs.log 2>&1 || { tail -20 build-resize2fs.log >&2; exit 1; }
echo "e2fsprogs: building mke2fs, tune2fs and dumpe2fs"
make -j"${JOBS:-4}" -C misc mke2fs tune2fs dumpe2fs RDYNAMIC= > build-mke2fs.log 2>&1 ||
    { tail -20 build-mke2fs.log >&2; exit 1; }

mkdir -p "$STAGE"
i686-elf-strip -o "$STAGE/e2fsck" e2fsck/e2fsck
echo "e2fsprogs: staged $STAGE/e2fsck ($(wc -c < "$STAGE/e2fsck" | tr -d ' ') bytes)"
i686-elf-strip -o "$STAGE/resize2fs" resize/resize2fs
echo "e2fsprogs: staged $STAGE/resize2fs ($(wc -c < "$STAGE/resize2fs" | tr -d ' ') bytes)"
for p in mke2fs tune2fs dumpe2fs; do
    i686-elf-strip -o "$STAGE/$p" misc/$p
    echo "e2fsprogs: staged $STAGE/$p ($(wc -c < "$STAGE/$p" | tr -d ' ') bytes)"
done
