# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# pinentry, which asks for a passphrase on behalf of gpg-agent: on a
# terminal with ncurses, or as a plain prompt on one that cannot draw. The
# graphical ones are left out. pinentry is a link to the curses one.
VERSION=1.3.3
DEPENDS="libgpg-error libassuan ncurses"
CONFIGURE_ARGS="--enable-pinentry-curses --enable-pinentry-tty --disable-pinentry-gtk2 --disable-pinentry-gnome3 --disable-pinentry-qt --disable-pinentry-qt5 --disable-pinentry-efl --disable-pinentry-fltk --disable-pinentry-tqt --disable-libsecret"
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"
