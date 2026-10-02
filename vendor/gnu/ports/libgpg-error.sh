# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libgpg-error, the error codes and the runtime GnuPG and its libraries
# share, with gpgrt-config, which the others' configure scripts ask for
# their flags and which reads the root's files through
# PKG_CONFIG_SYSROOT_DIR.
VERSION=1.61
CONFIGURE_ARGS="--disable-nls --disable-doc"
