# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU Bison, the parser generator, which runs m4 at /usr/bin/m4.
VERSION=3.8.2
DEPENDS="m4"
CONFIGURE_ARGS="--disable-nls M4=/usr/bin/m4"
