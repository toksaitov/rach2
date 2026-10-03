# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# jq, the JSON processor, with the Oniguruma its release carries for its
# regular expressions, and without its documentation's build, whose page
# the release has made.
VERSION=1.8.2
CONFIGURE_ARGS="--with-oniguruma=builtin --disable-docs --disable-maintainer-mode --disable-static"
