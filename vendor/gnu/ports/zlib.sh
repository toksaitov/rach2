# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# zlib, which OpenSSL, OpenSSH, curl and Git link against. Its configure
# script is its own rather than autoconf's: it takes the compiler from CC and
# the prefix of the other tools from CHOST, and builds the shared and the
# static library.
VERSION=1.3.2

port_build()
{
    CC="$TARGET-gcc" CHOST="$TARGET" "$SRC/configure" --prefix=/usr
    make
}
