# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU bash with the official patches. Its bundled readline reads the
# terminal's capabilities through ncurses, from the terminfo database the
# image has, since the bundled termcap reads /etc/termcap, which it has not;
# bash's own malloc is not used. configure runs test programs a cross build
# cannot, and five of its guesses are not what a build on Linux finds, so
# the recipe gives glibc's answers: getcwd allocates its buffer, which bash
# otherwise replaces with a walk of `..` that fails in a directory the user
# may not search; an exit status is the second byte of a wait status, which
# the last command of a pipeline under lastpipe is recorded with; printf
# has %a; the real-time signals are usable; and there are named pipes.
VERSION=5.3
PATCH_STRIP=0
PATCHES="bash-patch-001 bash-patch-002 bash-patch-003 bash-patch-004 bash-patch-005 bash-patch-006 bash-patch-007 bash-patch-008 bash-patch-009 bash-patch-010 bash-patch-011 bash-patch-012 bash-patch-013 bash-patch-014 bash-patch-015 bash-patch-016 bash-patch-017 bash-patch-018 bash-patch-019 bash-patch-020"
DEPENDS=ncurses
CONFIGURE_ARGS="--disable-nls --without-bash-malloc --with-curses bash_cv_termcap_lib=libncursesw \
bash_cv_getcwd_malloc=yes bash_cv_wexitstatus_offset=8 bash_cv_printf_a_format=yes \
bash_cv_unusable_rtsigs=no bash_cv_sys_named_pipes=present"
