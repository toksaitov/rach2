# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# OpenSSL for the linux-x86 target, with its shared libraries and zlib. Its
# Configure is a Perl program of its own, run by the build host's perl, and
# --cross-compile-prefix puts the target's prefix before the name of each
# tool the build runs. The configuration and the certificate store are in
# /etc/ssl. The test programs are not built, and the manual is installed as
# pages, without its HTML.
VERSION=4.0.2
DEPENDS=zlib

port_build()
{
    perl "$SRC/Configure" linux-x86 --cross-compile-prefix="$TARGET-" \
        --prefix=/usr --libdir=lib --openssldir=/etc/ssl shared zlib no-tests
    make
}

port_install()
{
    make install_sw install_ssldirs install_man_docs DESTDIR="$DEST"
}
