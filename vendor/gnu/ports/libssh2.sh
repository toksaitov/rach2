# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libssh2, the SSH library through which Chawan browses SFTP, with OpenSSL
# for its cryptography and zlib for compression, and without its examples.
VERSION=1.11.1
DEPENDS="openssl zlib"
CONFIGURE_ARGS="--with-crypto=openssl --with-libz --disable-examples-build --disable-docker-tests --disable-static"
