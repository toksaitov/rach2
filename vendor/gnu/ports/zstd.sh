# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Zstandard: zstd with its other names, unzstd, zstdcat and zstdmt, and
# libzstd, which rsync links. The build is upstream's Makefile, in the source
# tree, which links the programs with zlib and liblzma when a test program
# links against them, so that zstd reads and writes gzip and xz too, as
# Debian's does.
VERSION=1.5.7
DEPENDS="zlib xz"

port_build()
{
    make -C "$SRC" CC="$TARGET-gcc" AR="$TARGET-ar" PREFIX=/usr
}

port_install()
{
    make -C "$SRC" CC="$TARGET-gcc" AR="$TARGET-ar" PREFIX=/usr DESTDIR="$DEST" install
}
