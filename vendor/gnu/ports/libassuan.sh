# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Libassuan, the protocol GnuPG's programs talk to each other and to
# pinentry in.
VERSION=3.0.2
DEPENDS=libgpg-error
CONFIGURE_ARGS="--disable-doc"
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"
