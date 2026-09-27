# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU bash with the official patches. Its bundled readline reads the
# terminal's capabilities through ncurses, from the terminfo database the
# image has, since the bundled termcap reads /etc/termcap, which it has not;
# bash's own malloc is not used.
VERSION=5.3
PATCH_STRIP=0
PATCHES="bash-patch-001 bash-patch-002 bash-patch-003 bash-patch-004 bash-patch-005 bash-patch-006 bash-patch-007 bash-patch-008 bash-patch-009 bash-patch-010 bash-patch-011 bash-patch-012 bash-patch-013 bash-patch-014 bash-patch-015 bash-patch-016 bash-patch-017 bash-patch-018 bash-patch-019 bash-patch-020"
DEPENDS=ncurses
CONFIGURE_ARGS="--disable-nls --without-bash-malloc --with-curses bash_cv_termcap_lib=libncursesw"
