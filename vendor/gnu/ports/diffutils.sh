# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU diffutils: diff, cmp, diff3 and sdiff. gnulib's test of strcasecmp has
# no answer for a cross build to Linux and stops configure, so glibc's
# strcasecmp is declared working.
VERSION=3.12
CACHE="gl_cv_func_strcasecmp_works=yes"
CONFIGURE_ARGS="--disable-nls"
