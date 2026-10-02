# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Info-ZIP's unzip, with funzip, unzipsfx, zipgrep and zipinfo. Like zip, it
# is the last release with Debian's patches, which fix its known overflows
# and zip bombs, built in a copy of the source with the definitions Debian's
# build gives: names in UTF-8, large files, owners restored by number, and
# members compressed with bzip2.
VERSION=6.0
SOURCE_DIR=unzip60
DEBIAN=unzip-debian
DEPENDS=bzip2

port_build()
{
    cp -a "$SRC/." .
    make -f unix/Makefile CC="$TARGET-gcc" D_USE_BZ2=-DUSE_BZIP2 L_BZ2=-lbz2 \
        CF="-O2 -Wall -I. -DACORN_FTYPE_NFS -DWILD_STOP_AT_DIR -DLARGE_FILE_SUPPORT -DUNICODE_SUPPORT -DUNICODE_WCHAR -DUTF8_MAYBE_NATIVE -DNO_LCHMOD -DDATE_FORMAT=DF_YMD -DUSE_BZIP2 -DIZ_HAVE_UXUIDGID -DNOMEMCPY -DNO_WORKING_ISPRINT" \
        unzips
}

port_install()
{
    make -f unix/Makefile install prefix="$DEST/usr" MANDIR="$DEST/usr/share/man/man1"
}
