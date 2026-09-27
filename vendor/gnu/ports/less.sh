# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# less, the pager. It reads the terminal's capabilities through ncurses and
# searches with glibc's POSIX regular expressions.
VERSION=710
DEPENDS=ncurses
CONFIGURE_ARGS="--with-regex=posix"
