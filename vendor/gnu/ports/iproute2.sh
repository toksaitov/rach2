# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# iproute2: ip, which shows the machine's interfaces, addresses and routes
# through the kernel's netlink route socket, and ss, bridge and tc. Its
# configure is its own and takes the compiler from CC; it finds none of
# libmnl, libelf, libbpf, libcap or SELinux in the root, and the programs
# that need them are left out. The kernel answers netlink's requests that
# show and those that change a link's state, its addresses, the routes and
# the neighbours (netlink(7)). The programs are installed in /usr/sbin,
# which /sbin links into.
VERSION=7.2.0

port_build()
{
    cd "$SRC" || exit
    CC="$TARGET-gcc" ./configure --prefix=/usr
    make CC="$TARGET-gcc" HOSTCC=cc AR="$TARGET-ar"
}

port_install()
{
    cd "$SRC" || exit
    make install DESTDIR="$DEST" PREFIX=/usr SBINDIR=/usr/sbin
}
