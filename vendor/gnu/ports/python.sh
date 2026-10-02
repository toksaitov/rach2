# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Python 3, with the modules of its standard library that link the root's
# libraries: zlib, bz2 and lzma, ssl and hashlib with OpenSSL, ctypes with
# libffi, sqlite3, readline, curses, and uuid with util-linux's libuuid;
# pyexpat has its own copy of expat. dbm.gnu and tkinter are left out, since
# GDBM and Tk are not ported, and neither pip nor the test suite is
# installed, as Debian's python3 has neither. A cross build runs a Python of
# the same version on the build host, to generate code and to compile the
# byte code it installs, so the recipe builds one from the same source
# first. configure cannot look at the target's /dev/ptmx and /dev/ptc, so
# CACHE says the first exists, nor run its test of getaddrinfo, which
# glibc's passes. OpenSSL 4 has no SSLv3 and no methods of one protocol
# version, SSLv3_method and TLSv1_method to TLSv1_2_method, and does not say
# so in its headers, so the build defines the macros that leave them out of
# the ssl module.
VERSION=3.14.8
SOURCE_DIR=Python-3.14.8
DEPENDS="zlib bzip2 xz openssl libffi sqlite readline ncurses util-linux"
CONFIGURE_ARGS="--enable-shared --with-ensurepip=no --disable-test-modules"
CACHE="ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no"
NO_METHODS="-DOPENSSL_NO_SSL3 -DOPENSSL_NO_SSL3_METHOD -DOPENSSL_NO_TLS1_METHOD"
NO_METHODS="$NO_METHODS -DOPENSSL_NO_TLS1_1_METHOD -DOPENSSL_NO_TLS1_2_METHOD"

port_build()
{
    mkdir host
    (
        cd host || exit
        CC=cc "$SRC/configure" --without-ensurepip
        make
    )
    configure_autoconf --with-build-python="$WORK/host/python" CPPFLAGS="$NO_METHODS"
    make
}
