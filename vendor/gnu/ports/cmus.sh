# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# cmus, the music player for the terminal, and cmus-remote; it plays
# through OSS, /dev/dsp, and sets the PCM channel of /dev/mixer, and reads
# WAV itself and MP3, FLAC, Ogg Vorbis, Opus and AAC through FFmpeg's
# libraries. The source is the release Debian's archive has, with Debian's
# patches, which bring the FFmpeg input up to FFmpeg 8's interface. cmus's
# configure is its own: every feature is named, so nothing is taken from the
# build host or the root by chance, and its test for OSS, which looks for
# the build host's /usr/include/sys/soundcard.h, compiles the header with the
# cross compiler instead. Its manual pages are made by Doc/ttman, which must
# run on the build host and so is built with the host's compiler. Its
# Makefile takes the version from `git describe`, which in a build tree
# inside a clone describes the clone, so the version is given to make. Its
# FFmpeg input finds FFmpeg's libraries in /usr/lib/ffmpeg by its run path.
VERSION=2.12.0
DEPENDS="ncurses ffmpeg"
DEBIAN=cmus-debian

port_build()
{
    cp -a "$SRC/." .
    sed -i 's|^\tif test -f /usr/include/sys/soundcard.h$|\tif try_compile "#include <sys/soundcard.h>"|' configure
    grep -q '^	if try_compile "#include <sys/soundcard.h>"$' configure || return 1
    ./configure prefix=/usr DEBUG=0 CROSS="$TARGET-" HOSTCC=cc \
        PKG_CONFIG="$TARGET-pkg-config" LDFLAGS=-Wl,-rpath,/usr/lib/ffmpeg \
        CONFIG_OSS=y CONFIG_FFMPEG=y CONFIG_WAV=y CONFIG_CUE=y \
        CONFIG_AAC=n CONFIG_AAUDIO=n CONFIG_ALSA=n CONFIG_AO=n CONFIG_ARTS=n \
        CONFIG_CDDB=n CONFIG_CDIO=n CONFIG_COREAUDIO=n CONFIG_DISCID=n \
        CONFIG_FLAC=n CONFIG_JACK=n CONFIG_MAD=n CONFIG_MIKMOD=n CONFIG_BASS=n \
        CONFIG_MODPLUG=n CONFIG_MP4=n CONFIG_MPC=n CONFIG_MPRIS=n CONFIG_OPUS=n \
        CONFIG_PULSE=n CONFIG_ROAR=n CONFIG_SAMPLERATE=n CONFIG_SNDIO=n \
        CONFIG_SUN=n CONFIG_TREMOR=n CONFIG_VORBIS=n CONFIG_VTX=n \
        CONFIG_WAVEOUT=n CONFIG_WAVPACK=n
    cc -O2 -o Doc/ttman Doc/ttman.c
    make VERSION="$VERSION"
}

port_install()
{
    make install VERSION="$VERSION" DESTDIR="$DEST"
}
