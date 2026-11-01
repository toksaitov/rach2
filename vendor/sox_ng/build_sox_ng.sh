#!/bin/sh
# Build sox_ng with built-in WAV and OSS against Rach2's static musl.
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

GOT=$("$REPO/build/tools/verify/sha256" "$TARBALL" | awk '{print $1}')
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "sox_ng: sha256 mismatch for $TARBALL" >&2
    exit 1
fi
mkdir -p "$WORK" "$STAGE"
rm -rf "$SRC"
tar xzf "$TARBALL" -C "$WORK"
BUILD_TRIPLE=$(sh "$SRC/config.guess")
(
    cd "$SRC"
    CC="$WRAP" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip" \
        PKG_CONFIG=false CFLAGS=-O2 LDFLAGS="-static -Wl,-z,noexecstack" \
        ./configure --build="$BUILD_TRIPLE" --host="$HOST_TRIPLE" \
        --prefix=/usr --disable-shared --enable-static --disable-symlinks \
        --disable-stack-protector --disable-openmp --without-libltdl \
        --without-magic --without-png --without-fftw --without-speexdsp \
        --without-ladspa --without-ffmpeg --without-mad --without-id3tag \
        --without-lame --without-twolame --without-dolbyb --without-ebur128 \
        --without-oggvorbis --without-opus --without-flac --without-amrwb \
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
cp "$SRC/COPYING" "$STAGE/COPYING"
printf 'sox_ng: result: built %s for %s\n' "$VERSION" "$RACH2_ARCH"
