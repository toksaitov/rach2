# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU tar. It runs a compressor through /bin/sh, and gzip, xz and lzma are
# named by their paths, so that tar -cz and -cJ run the GNU programs whatever
# the caller's path holds.
VERSION=1.35
CONFIGURE_ARGS="--disable-nls --with-gzip=/usr/bin/gzip --with-xz=/usr/bin/xz --with-lzma=/usr/bin/lzma"
