# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU grep, with -P through PCRE2.
VERSION=3.12
DEPENDS="pcre2"
CONFIGURE_ARGS="--disable-nls --enable-perl-regexp"
