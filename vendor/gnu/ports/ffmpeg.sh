# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# FFmpeg and ffprobe, with OSS input and output and the built-in audio
# decoders for WAV, MP3, FLAC, Ogg Vorbis, Opus and AAC.
# Shared libraries serve cmus; no external library is detected or enabled.
VERSION=9.0.2

port_build()
{
    "$SRC/configure" --prefix=/usr --enable-cross-compile \
        --target-os=linux --arch="$RACH2_ARCH" \
        --cross-prefix="$TARGET-" --sysroot="$ROOT" \
        --pkg-config=false --disable-autodetect --disable-iconv --enable-pthreads \
        --disable-x86asm --enable-shared --enable-pic --disable-static \
        --disable-gpl --disable-nonfree --disable-version3 \
        --disable-doc --disable-debug --disable-ffplay --disable-network \
        --disable-swscale --extra-ldflags=-Wl,-z,noexecstack \
        --disable-everything \
        --enable-indev=oss --enable-outdev=oss \
        --enable-protocol=file,pipe \
        --enable-demuxer=wav,mp3,flac,ogg,aac,mov,pcm_s16le \
        --enable-muxer=wav,pcm_s16le \
        --enable-decoder=mp3,mp3float,flac,vorbis,opus,aac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le \
        --enable-encoder=pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le \
        --enable-parser=mpegaudio,flac,vorbis,opus,aac \
        --enable-filter=aresample,aformat,anull,abuffer,abuffersink,volume \
        --enable-swresample
    make
}

port_install()
{
    make install DESTDIR="$DEST"
    install -Dm644 "$SRC/COPYING.LGPLv2.1" \
        "$DEST/usr/share/licenses/ffmpeg/COPYING.LGPLv2.1"
}
