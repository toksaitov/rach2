# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# cronie, the cron daemon Fedora and Debian have, with crontab(1). init
# starts crond at boot where the root has it. The crontabs are in
# /var/spool/cron/crontabs, where Debian keeps them, and crontab is set-id
# to the group crontab, which may write that directory, as Debian's cronie
# is: the manifest installs it with that bit from build/vendor/gnu/setid,
# since a file the overlay adds has none, and the port keeps it out of the
# root. An empty /etc/cron.deny lets every user have a crontab, which
# cronie allows otherwise only to root. There is no PAM, no SELinux and no
# anacron. cronie's declarations have the empty parameter lists that C23
# reads as taking no arguments, so it is compiled as C17.
VERSION=1.7.2
CONFIGURE_ARGS="--sysconfdir=/etc --localstatedir=/var --disable-anacron --with-inotify --without-pam --without-selinux --without-audit --with-editor=/usr/bin/vi"
CACHE="SPOOL_DIR=/var/spool/cron/crontabs"

port_build()
{
    configure_autoconf CFLAGS="-O2 -std=gnu17"
    make
}

port_install()
{
    make install DESTDIR="$DEST"
    mkdir -p "$DEST/etc"
    : >"$DEST/etc/cron.deny"
    mkdir -p "$OUT/setid"
    mv "$DEST/usr/bin/crontab" "$OUT/setid/crontab"
    "$TARGET-strip" --strip-debug "$OUT/setid/crontab"
}
