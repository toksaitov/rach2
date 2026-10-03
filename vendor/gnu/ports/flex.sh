# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# flex, the scanner generator, which runs m4 at /usr/bin/m4. A cross build
# cannot run the stage-one flex it builds to make its own scanner, so the
# make is given the build host's flex for that step. configure cannot test
# in a cross build whether malloc(0) and realloc(0) return pointers, which
# glibc's do. The stage-one flex is built with flex's replacement malloc,
# which declares malloc without a prototype, and C23, GCC's default, reads
# that as a function of no arguments, so flex is compiled as C17.
VERSION=2.6.4
DEPENDS="m4"
CONFIGURE_ARGS="--disable-nls --disable-static M4=/usr/bin/m4"
CACHE="ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes"

port_build()
{
    configure_autoconf "CFLAGS=-O2 -std=gnu17"
    make STAGE1FLEX=flex CFLAGS="-O2 -std=gnu17"
}
