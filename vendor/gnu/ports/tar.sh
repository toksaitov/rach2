# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU tar. It runs a compressor through /bin/sh, BusyBox's ash, which runs
# its own applet for a bare name before it searches the path, and BusyBox's
# xz only decompresses; so gzip, xz and lzma are named by their paths, and
# tar -cz and -cJ run the GNU programs.
VERSION=1.35
CONFIGURE_ARGS="--disable-nls --with-gzip=/usr/bin/gzip --with-xz=/usr/bin/xz --with-lzma=/usr/bin/lzma"
