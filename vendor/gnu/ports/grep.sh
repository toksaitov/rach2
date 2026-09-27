# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU grep. PCRE2 is not ported, so grep is built without -P.
VERSION=3.12
CONFIGURE_ARGS="--disable-nls --disable-perl-regexp"
