# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# tmux, the terminal multiplexer. configure would choose the TERM its
# windows get by running a program that asks ncurses for each candidate,
# which a cross build cannot run; the root's terminfo database has
# tmux-256color, the one Debian's tmux gives.
VERSION=3.7c
DEPENDS="libevent ncurses"
CONFIGURE_ARGS="--with-TERM=tmux-256color"

# tmux's page is dated $Mdocdate$, which mandoc renders as the day it runs;
# it gets the date its file has in the release, so that two builds of the
# root render it alike.
port_install()
{
    make install DESTDIR="$DEST"
    d=$(date -u -r "$SRC/tmux.1" '+%B %-d, %Y')
    sed -i "s/^\.Dd \$Mdocdate\$\$/.Dd $d/" "$DEST/usr/share/man/man1/tmux.1"
    grep -q "^\.Dd $d\$" "$DEST/usr/share/man/man1/tmux.1"
}
