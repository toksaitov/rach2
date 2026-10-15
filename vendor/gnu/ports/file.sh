# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# file, which names what a file holds, and libmagic, through which it reads
# the compiled magic database. The database is compiled during the build by
# a file of the same version, which a cross build looks for on the build
# host's path; the recipe gives it the file it has just built instead, run
# through the root's loader as Perl's recipe runs its programs. Files that
# zlib and xz compressed are looked into with -z; the sandbox file makes
# with libseccomp is left out, since libseccomp is not ported.
VERSION=5.48
DEPENDS="zlib xz"
CONFIGURE_ARGS="--enable-zlib --enable-xzlib --disable-bzlib --disable-zstdlib --disable-lzlib
--disable-lrziplib --disable-libseccomp"

port_build()
{
    configure_autoconf
    # shellcheck disable=SC2046 # one word or none
    make -C src $(libtool_static)
    built="$WORK/src/.libs/file"
    [ -f "$built" ] || built="$WORK/src/file"
    # shellcheck disable=SC2046 # one word or none
    make $(libtool_static) \
        FILE_COMPILE="$ROOT$LOADER --library-path $WORK/src/.libs:$ROOT/lib:$ROOT/usr/lib $built"
}
