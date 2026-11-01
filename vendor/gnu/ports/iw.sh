# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# iw: shows and sets a wireless card's state through nl80211, with libnl's
# generic netlink. Its build is a Makefile alone, run in the source tree,
# which asks pkg-config for libnl. Its version is the release's, since the
# source tree has no .git for its version.sh to ask. It installs in
# /usr/sbin, which /sbin links into. rfkill is util-linux's.
VERSION=6.17
DEPENDS=libnl

iw_make()
{
    make -C "$SRC" CC="$TARGET-gcc" PKG_CONFIG="$TARGET-pkg-config" PREFIX=/usr \
        SBINDIR=/usr/sbin "$@"
}

port_build()
{
    iw_make
}

port_install()
{
    iw_make install DESTDIR="$DEST"
}
