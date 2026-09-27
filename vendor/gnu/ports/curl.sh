# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# curl and libcurl, with OpenSSL for TLS and zlib for compressed transfers.
# The optional libraries that are not ported are declined by name, since
# configure stops when libpsl is missing. No CA bundle or directory is
# given, so curl uses OpenSSL's default store under /etc/ssl.
VERSION=8.22.0
DEPENDS="openssl zlib"
CONFIGURE_ARGS="--with-openssl --with-zlib --without-ca-bundle --without-ca-path --with-ca-fallback --without-libpsl --without-brotli --without-zstd --without-libidn2 --without-nghttp2 --without-ngtcp2 --without-nghttp3 --without-libgsasl --disable-ldap"
