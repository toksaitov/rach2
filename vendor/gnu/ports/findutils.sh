# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU findutils: find, xargs, locate and updatedb. The locate database is
# kept in /var/lib/locate rather than in /usr/var, the default.
VERSION=4.11.0
CONFIGURE_ARGS="--disable-nls --localstatedir=/var/lib/locate"
