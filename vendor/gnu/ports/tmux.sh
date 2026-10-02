# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# tmux, the terminal multiplexer. configure would choose the TERM its
# windows get by running a program that asks ncurses for each candidate,
# which a cross build cannot run; the root's terminfo database has
# tmux-256color, the one Debian's tmux gives.
VERSION=3.7c
DEPENDS="libevent ncurses"
CONFIGURE_ARGS="--with-TERM=tmux-256color"
