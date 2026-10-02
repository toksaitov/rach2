# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Info-ZIP's zip, zipcloak, zipnote and zipsplit. Info-ZIP makes no more
# releases and signs nothing; the source is its last release, whose digest
# Debian's archive records, with Debian's patches, which fix its known
# overflows and its build with current compilers. It is built as Debian
# builds it, in a copy of the source: unix/configure writes the flags for
# unix/Makefile. It runs a test program to learn whether files may pass
# 2 GiB, which a cross build may not be able to run, so the flag for large
# files is added to what it wrote when it is not there.
VERSION=3.0
SOURCE_DIR=zip30
DEBIAN=zip-debian

port_build()
{
    cp -a "$SRC/." .
    sh unix/configure "$TARGET-gcc" "-O2 -Wall -I. -DUNIX"
    if ! grep -q -- -DLARGE_FILE_SUPPORT flags; then
        sed 's/CFLAGS="/CFLAGS="-DLARGE_FILE_SUPPORT /' flags >flags.new
        mv flags.new flags
    fi
    eval "make -f unix/Makefile zips $(cat flags)"
}

port_install()
{
    make -f unix/Makefile install prefix="$DEST/usr" MANDIR="$DEST/usr/share/man/man1"
}
