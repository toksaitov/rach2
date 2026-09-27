# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU grep. PCRE2 is not ported, so grep is built without -P. egrep and
# fgrep run grep by its bare name, so configure is told to start them with
# /bin/bash: under /bin/sh, BusyBox's shell, a bare name finds BusyBox's
# applet before it searches the path.
VERSION=3.12
CACHE="CONFIG_SHELL=/bin/bash"
CONFIGURE_ARGS="--disable-nls --disable-perl-regexp"
