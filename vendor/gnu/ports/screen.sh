# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU screen, the terminal multiplexer, without PAM, which the root does not
# have, and not set-id: a user's sessions are sockets in ~/.screen, as
# screen keeps them when it is not installed set-id root. Its Makefile makes
# headers in the source directory, so it is built in a copy of the tree.
VERSION=5.0.2
DEPENDS="ncurses"
CONFIGURE_ARGS="--enable-pam=no --with-system_screenrc=/etc/screenrc --with-socket-dir=no"

port_build()
{
    cp -a "$SRC/." .
    (
        SRC=.
        configure_autoconf
    )
    make
}
