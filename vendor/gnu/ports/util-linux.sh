# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# util-linux: the system's tools a Debian machine has from it, of which
# BusyBox's applets have fewer options or none: last, lsblk, findmnt, flock,
# prlimit, ionice, column, script, newgrp, dmesg, fdisk, lscpu and the rest.
# The programs that log a user in or change one's identity are left to
# OpenSSH and BusyBox, and so are the ones that need a library the root does
# not have: the lastlog database's SQLite, libcap-ng's setpriv, readline,
# libudev, systemd, SELinux, the audit library, cryptsetup and Python. kill
# is procps-ng's. newgrp is set-id root, as Debian's is, and write and wall
# set-id to the group tty, whose pseudo-terminals they write to: the
# manifest installs them with those bits from build/vendor/gnu/setid, since a
# file the overlay adds has none, and the port keeps them out of the root.
# Nothing else is set-id, and the manual pages are the ones the release
# carries, so
# asciidoctor is not needed. For a prefix of /usr, util-linux would put its
# programs and libraries in /bin, /sbin and /lib, which are the musl
# system's; they go under /usr, as the other ports' do and Debian's merged
# /usr has them.
VERSION=2.42.4
DEPENDS=ncurses
CONFIGURE_ARGS="--bindir=/usr/bin --sbindir=/usr/sbin --libdir=/usr/lib --disable-nls --disable-static --disable-asciidoc --disable-poman \
--disable-bash-completion --disable-makeinstall-chown --disable-makeinstall-setuid \
--enable-write --disable-liblastlog2 --disable-pam-lastlog2 --disable-login \
--disable-su --disable-runuser --disable-chfn-chsh --disable-sulogin --disable-nologin \
--disable-setpriv --disable-kill --disable-uuidd --without-python --without-systemd \
--without-udev --without-selinux --without-audit --without-readline --without-cap-ng \
--without-btrfs --without-cryptsetup --without-econf --without-libmagic --enable-newgrp"

port_install()
{
    make install DESTDIR="$DEST"
    mkdir -p "$OUT/setid"
    for p in newgrp write wall; do
        mv "$DEST/usr/bin/$p" "$OUT/setid/$p"
        "$TARGET-strip" --strip-debug "$OUT/setid/$p"
    done
}
