# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU Autoconf, which writes a package's configure script. Its programs are
# Perl and shell scripts that run m4, whose paths configure writes into them:
# it is given /usr/bin/m4 and /usr/bin/perl, where the build host and the
# root both have them, and its build runs them on the build host to freeze
# the macros it installs.
VERSION=2.73
DEPENDS="m4 perl"
CONFIGURE_ARGS="M4=/usr/bin/m4 PERL=/usr/bin/perl"
