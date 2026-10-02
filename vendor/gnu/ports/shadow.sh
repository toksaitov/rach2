# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# shadow, the account tools Debian has from it that BusyBox has not:
# useradd, usermod and userdel, groupadd, groupmod and groupdel, gpasswd,
# chsh, chfn and chage, chgpasswd, newusers, vipw and vigr, pwck and grpck,
# faillog, and the conversions to and from the shadow files, with
# /etc/login.defs. passwd, su, login, chpasswd and nologin stay BusyBox's
# and newgrp util-linux's, so the port keeps its own out of the root. chsh,
# chfn and gpasswd are set-id root, as Debian's are: the manifest installs
# them with that bit from build/vendor/gnu/setid, since a file the overlay
# adds has none. There is no PAM, SELinux, ACL, audit, tcb, logind, nscd,
# sssd or libbsd, and no subordinate ids, which newuidmap and newgidmap
# would need, or lastlog, which shadow builds only on request.
VERSION=4.20.2
CONFIGURE_ARGS="--without-libpam --without-selinux --without-acl --without-attr --without-audit \
--without-tcb --without-nscd --without-sssd --without-libbsd --without-su --without-btrfs \
--disable-logind --disable-subordinate-ids --disable-nls --disable-static \
--with-group-name-max-length=32"

port_install()
{
    make install DESTDIR="$DEST"
    rm -f "$DEST/bin/login" "$DEST/usr/bin/passwd" "$DEST/usr/bin/newgrp" "$DEST/usr/bin/sg" \
        "$DEST/usr/sbin/chpasswd" "$DEST/usr/sbin/nologin"
    rmdir "$DEST/bin"
    rm -f "$DEST"/usr/share/man/man1/login.1 "$DEST"/usr/share/man/man1/passwd.1 \
        "$DEST"/usr/share/man/man1/newgrp.1 "$DEST"/usr/share/man/man1/sg.1 \
        "$DEST"/usr/share/man/man8/chpasswd.8 "$DEST"/usr/share/man/man8/nologin.8
    mkdir -p "$OUT/setid"
    for p in chsh chfn gpasswd; do
        mv "$DEST/usr/bin/$p" "$OUT/setid/$p"
        "$TARGET-strip" --strip-debug "$OUT/setid/$p"
    done
}
