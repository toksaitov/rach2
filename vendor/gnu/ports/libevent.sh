# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libevent, the event loop tmux is built on, without its OpenSSL layer and
# its sample programs.
VERSION=2.1.13-stable
CONFIGURE_ARGS="--disable-openssl --disable-samples --disable-libevent-regress"
