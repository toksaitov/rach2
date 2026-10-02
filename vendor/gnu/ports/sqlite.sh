# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# SQLite: the library, which Python's sqlite3 module and GnuPG's keyboxd
# link, and the sqlite3 shell with readline, from the amalgamation's
# release, with the full-text, R-tree, session and statistics extensions
# Debian's has. Its configure is autosetup's, which is built with the
# build host's compiler and run in a copy of the source. In a cross build it
# looks for readline only where it is told, so it is given the root's
# header and the libraries to link.
VERSION=3.53.4
SOURCE_DIR=sqlite-autoconf-3530400
DEPENDS=readline
CONFIGURE_ARGS="--all --disable-static --with-readline-header=$ROOT/usr/include/readline/readline.h --with-readline-ldflags=-lreadline"


port_build()
{
    cp -a "$SRC/." .
    # shellcheck disable=SC2086 # CONFIGURE_ARGS is a list of words
    CC="$TARGET-gcc" CC_FOR_BUILD=cc ./configure --host="$TARGET" --build="$BUILD" \
        --prefix=/usr $CONFIGURE_ARGS
    make
}
