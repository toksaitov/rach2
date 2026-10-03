# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU Automake, which writes a package's Makefile.in. Its configure stops
# without an Autoconf to run, which the build host does not have, so the
# recipe first builds Autoconf for the build host in the work directory from
# the same tarball the autoconf port uses, and puts it first in the path of
# automake's configure and make. Given no path, configure writes the bare
# names autoconf and autom4te into the scripts automake installs, which then
# run the root's, and /usr/bin/perl.
VERSION=1.19
DEPENDS="autoconf perl"

port_build()
{
    tar -xf "$("$DIR/fetch.sh" --path autoconf)" -C "$WORK"
    (
        cd "$WORK"/autoconf-* || exit 1
        ./configure --prefix="$WORK/host" M4=/usr/bin/m4 PERL=/usr/bin/perl >/dev/null
        make >/dev/null
        make install >/dev/null
    )
    (
        PATH="$WORK/host/bin:$PATH"
        configure_autoconf PERL=/usr/bin/perl
        make
    )
}
