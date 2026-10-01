# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# PCRE2, the library of Perl's regular expressions, which GNU grep's -P and
# Git's grep -P match with, and its pcre2grep and pcre2test. It is built
# with its just-in-time compiler, which writes a pattern's machine code into
# memory it maps for the purpose.
VERSION=10.49
CONFIGURE_ARGS="--enable-jit"
