# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU Readline, with its released patches, linked with ncurses for the
# terminal's capabilities, which Python's interactive prompt and the SQLite
# and GnuPG programs read lines with. bash has its own copy.
VERSION=8.3
PATCHES="readline-patch-001 readline-patch-002 readline-patch-003 readline-patch-004 readline-patch-005 readline-patch-006"
PATCH_STRIP=0
DEPENDS=ncurses
CONFIGURE_ARGS="--with-curses --disable-static"

port_build()
{
    configure_autoconf
    make SHLIB_LIBS=-lncursesw
}

port_install()
{
    make install DESTDIR="$DEST" SHLIB_LIBS=-lncursesw
}
