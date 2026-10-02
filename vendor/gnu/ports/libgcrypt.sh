# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Libgcrypt, the cryptography of GnuPG. Its configure, like the other GnuPG
# libraries', finds libgpg-error through gpgrt-config, which CACHE names in
# the root, since the build host has none.
VERSION=1.12.4
DEPENDS=libgpg-error
CONFIGURE_ARGS="--disable-doc"
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"
