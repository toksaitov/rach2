# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libnl: the netlink libraries wpa_supplicant and iw use to speak nl80211,
# libnl-3, libnl-genl-3 and libnl-route-3 with the rest of the family, shared
# only. The nl-* programs are not built, nor their pages installed, since
# iproute2's ip shows the same. The build host's flex and bison make the
# parsers of its packet classifier, which the tarball does not carry made.
# Its classid and pktloc tables are in /etc/libnl.
VERSION=3.12.0
CONFIGURE_ARGS="--disable-static --enable-cli=no"

port_install()
{
    make install DESTDIR="$DEST"
    rm -rf "$DEST/usr/share/man"
}
