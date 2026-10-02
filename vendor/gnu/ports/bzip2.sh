# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# bzip2 and libbz2, which Python's bz2 module, unzip and tar -j use. Its
# build is two makefiles, in a copy of the source: Makefile-libbz2_so makes
# the shared library and a bzip2 linked with it, and Makefile the static
# library and the other programs. The install puts the manual pages under
# /usr/share/man and makes the links relative, as Linux From Scratch does,
# and bzip2 is the one linked with the shared library.
VERSION=1.0.8

port_build()
{
    cp -a "$SRC/." .
    # shellcheck disable=SC2016 # the dollars are make's
    sed -e 's@\(ln -s -f \)$(PREFIX)/bin/@\1@' -e 's@(PREFIX)/man@(PREFIX)/share/man@g' \
        Makefile >Makefile.rach2
    mv Makefile.rach2 Makefile
    make -f Makefile-libbz2_so CC="$TARGET-gcc"
    make clean
    make CC="$TARGET-gcc" AR="$TARGET-ar" RANLIB="$TARGET-ranlib"
}

port_install()
{
    make install PREFIX="$DEST/usr"
    cp -a libbz2.so.1.0.8 libbz2.so.1.0 "$DEST/usr/lib/"
    ln -s libbz2.so.1.0.8 "$DEST/usr/lib/libbz2.so"
    cp bzip2-shared "$DEST/usr/bin/bzip2"
}
