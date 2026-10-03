# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# OpenSSL for its linux-x86 or linux-x86_64 target, with its shared libraries
# and zlib, and the libraries in /usr/lib on both. Its Configure is a Perl
# program of its own, run by the build host's perl, and --cross-compile-prefix
# puts the target's prefix before the name of each tool the build runs. The
# configuration and the certificate store are in /etc/ssl. The test programs
# are not built, and the manual is installed as pages, without its HTML.
VERSION=4.0.2
DEPENDS=zlib

port_build()
{
    config=linux-x86
    [ "$RACH2_ARCH" = x86_64 ] && config=linux-x86_64
    perl "$SRC/Configure" "$config" --cross-compile-prefix="$TARGET-" \
        --prefix=/usr --libdir=lib --openssldir=/etc/ssl shared zlib no-tests
    make
}

port_install()
{
    make install_sw install_ssldirs install_man_docs DESTDIR="$DEST"
}
