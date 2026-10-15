# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Vim for the terminal, with its huge feature set and ncurses for the
# terminal's capabilities; its configure runs in the source tree. configure
# cannot run its test programs in a cross build, so two answers are given:
# stat of "file/" fails, as on Linux, and timer_create is declared not to
# work, so Vim times its searches with setitimer. The kernel's file systems
# keep no extended attributes, and Vim is built without them.
VERSION=9.2.1132
DEPENDS=ncurses
CACHE="vim_cv_stat_ignores_slash=no vim_cv_timer_create_works=no"
CONFIGURE_ARGS="--with-features=huge --enable-gui=no --without-x --with-tlib=ncursesw --enable-gpm=no --disable-sysmouse --disable-nls --disable-canberra --disable-libsodium --disable-selinux --disable-smack --disable-xattr"

port_build()
{
    cd "$SRC" || exit
    configure_autoconf
    make
}

port_install()
{
    cd "$SRC" || exit
    make install DESTDIR="$DEST"
}
