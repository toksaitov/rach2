# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# sudo, which runs a command as another user by the rules of /etc/sudoers,
# checked against the shadow file, since the root has no PAM, and without its
# log server and the mail it would send. sudo is set-id root: its recipe
# leaves it in setid/ of the layer's build tree, from which the manifest
# installs it with that bit, and the manifest gives /etc/sudoers its mode 0440
# and Debian's rules, root's and the group sudo's. Its install makes
# /run/sudo, which sudo makes again when it needs it; the root's /run is a
# link to /var/run, which a directory there would take the place of.
VERSION=1.9.17p2
CONFIGURE_ARGS="--without-pam --with-passwd --without-sendmail --disable-log-server \
--disable-log-client --with-rundir=/run/sudo --with-vardir=/var/lib/sudo \
--with-env-editor --with-secure-path=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
--disable-static --enable-tmpfiles.d=no --with-logfac=authpriv"

port_install()
{
    make install DESTDIR="$DEST" INSTALL_OWNER= INSTALL_BACKUP=
    rm -f "$DEST/etc/sudoers" "$DEST/etc/sudoers.dist"
    rm -rf "${DEST:?}/run"
    mkdir -p "$OUT/setid"
    mv "$DEST/usr/bin/sudo" "$OUT/setid/sudo"
    "$TARGET-strip" --strip-debug "$OUT/setid/sudo"
    rm -f "$DEST/usr/bin/sudoedit"
}
