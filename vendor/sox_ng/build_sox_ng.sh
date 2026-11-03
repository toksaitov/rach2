#!/bin/sh
# Build sox_ng with built-in WAV and OSS, and FLAC through a static libFLAC,
# against Rach2's static musl.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
VERSION="14.8.1"
TARBALL="$SCRIPT_DIR/sox_ng-$VERSION.tar.gz"
TARBALL_SHA256="1dedd9ad574abc576bbda06743867fb778accf98a1eacde1a42913acbf072448"
WORK="$ARCH_BUILD/vendor-work/sox_ng"
SRC="$WORK/sox_ng-$VERSION"
STAGE="$ARCH_BUILD/vendor/sox_ng"
WRAP="$REPO/vendor/musl/muslgcc"
FLAC_VERSION="1.5.0"
FLAC_TARBALL="$SCRIPT_DIR/flac-$FLAC_VERSION.tar.xz"
FLAC_TARBALL_SHA256="f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920"
FLAC_SRC="$WORK/flac-$FLAC_VERSION"
FLAC_ROOT="$WORK/flac-root"

check_sha256() {
    got=$("$REPO/build/tools/verify/sha256" "$1" | awk '{print $1}')
    if [ "$got" != "$2" ]; then
        echo "sox_ng: sha256 mismatch for $1" >&2
        exit 1
    fi
}
check_sha256 "$TARBALL" "$TARBALL_SHA256"
check_sha256 "$FLAC_TARBALL" "$FLAC_TARBALL_SHA256"
mkdir -p "$WORK" "$STAGE"
rm -rf "$SRC" "$FLAC_SRC" "$FLAC_ROOT"
tar xzf "$TARBALL" -C "$WORK"
tar -xf "$FLAC_TARBALL" -C "$WORK"
BUILD_TRIPLE=$(sh "$SRC/config.guess")

# libFLAC alone, static, without Ogg FLAC, threads or the flac programs,
# installed under the work directory for sox_ng's configure to find.
(
    cd "$FLAC_SRC"
    CC="$WRAP" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip" \
        PKG_CONFIG=false CFLAGS="-Os -ffunction-sections -fdata-sections" \
        ./configure --build="$BUILD_TRIPLE" --host="$HOST_TRIPLE" \
        --prefix="$FLAC_ROOT" --disable-shared --enable-static \
        --disable-ogg --disable-oggtest --disable-programs --disable-examples \
        --disable-cpplibs --disable-doxygen-docs --disable-multithreading \
        --disable-stack-smash-protection --disable-fortify-source \
        --disable-version-from-git --disable-thorough-tests
)
make -C "$FLAC_SRC/src/libFLAC" install
make -C "$FLAC_SRC/include" install

(
    cd "$SRC"
    CC="$WRAP" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip" \
        PKG_CONFIG=false CFLAGS="-Os -ffunction-sections -fdata-sections" \
        CPPFLAGS="-I$FLAC_ROOT/include" \
        LDFLAGS="-static -L$FLAC_ROOT/lib -Wl,--gc-sections -Wl,-z,noexecstack" \
        ./configure --build="$BUILD_TRIPLE" --host="$HOST_TRIPLE" \
        --prefix=/usr --disable-shared --enable-static --disable-symlinks \
        --disable-stack-protector --disable-openmp --without-libltdl \
        --without-magic --without-png --without-fftw --without-speexdsp \
        --without-ladspa --without-ffmpeg --without-mad --without-id3tag \
        --without-lame --without-twolame --without-dolbyb --without-ebur128 \
        --without-oggvorbis --without-opus --with-flac --without-amrwb \
        --without-amrnb --without-wavpack --without-sndio --without-coreaudio \
        --without-alsa --without-ao --without-pulseaudio --without-waveaudio \
        --without-sndfile --without-sunaudio --without-mp3 --without-gsm \
        --without-lpc10 --with-oss=yes
)
# The upstream link names these archives even with their formats disabled.
make -C "$SRC/lpc10" all
make -C "$SRC/libgsm" all
make -C "$SRC/src" sox_ng
"${CROSS}strip" "$SRC/src/sox_ng" -o "$STAGE/sox"
"$WRAP" -O2 -Wall -Wextra -Werror -ansi -Wl,-z,noexecstack "$SCRIPT_DIR/mode.c" -o "$WORK/mode"
"${CROSS}strip" "$WORK/mode" -o "$STAGE/mode"
"$WRAP" -Os -Wall -Wextra -Werror -ansi -Wl,-z,noexecstack \
    -DMULTICALL_PATH='"/bin/busybox"' "$SCRIPT_DIR/mode.c" -o "$WORK/sh"
"${CROSS}strip" "$WORK/sh" -o "$STAGE/sh"
# One notice for the program: sox_ng's GPL, then libFLAC's BSD license.
{
    cat "$SRC/COPYING"
    printf '\n\nThe FLAC format is read and written by libFLAC %s, under this license:\n\n' "$FLAC_VERSION"
    cat "$FLAC_SRC/COPYING.Xiph"
} >"$STAGE/COPYING"
printf 'sox_ng: result: built %s for %s\n' "$VERSION" "$RACH2_ARCH"
