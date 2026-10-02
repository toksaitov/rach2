# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU Texinfo: info, the reader of the manuals in /usr/share/info,
# install-info, and makeinfo with the rest of texi2any, which is Perl and
# runs with the root's perl. Its modules in C, which a cross build would
# compile against the build host's Perl, are left out, and texi2any uses
# their Perl versions. configure sets up a second build of the tools the
# build itself runs, with the build host's compiler, in tools/; it leaves
# out info's makedoc, which writes a header info is compiled with, when the
# build host has no ncurses of its own, so port_build makes it there.
VERSION=7.3
DEPENDS="ncurses perl"
CONFIGURE_ARGS="--disable-nls --disable-perl-xs"

port_build()
{
    configure_autoconf
    make -C tools
    make -C tools/info makedoc
    make
}
