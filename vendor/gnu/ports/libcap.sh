# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libcap, the library of POSIX capabilities, which BIND needs on Linux, and
# its getcap, setcap and capsh, built by its Makefiles, without PAM's module
# and Go's bindings; a program the build runs on the build host is compiled
# with the host's compiler.
VERSION=2.75
SOURCE=libcap

libcap_make()
{
    make -C "$SRC" CC="$TARGET-gcc" BUILD_CC=cc AR="$TARGET-ar" RANLIB="$TARGET-ranlib" \
        prefix=/usr lib=lib sbindir=sbin GOLANG=no PAM_CAP=no "$@"
}

port_build()
{
    libcap_make
}

port_install()
{
    libcap_make DESTDIR="$DEST" RAISE_SETFCAP=no install
    rm -f "$DEST"/usr/lib/libcap.a "$DEST"/usr/lib/libpsx.a
}
