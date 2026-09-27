# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# OpenSSH: the server, the client, ssh-keygen, sftp-server and the rest, with
# OpenSSL for its cryptography and zlib for compression. The configuration is
# in /etc/ssh, the server's privilege-separated child runs as the user sshd
# in /var/empty, and there is no PAM. The programs are installed without the
# build host's strip and without host keys, which the target makes itself.
VERSION=10.5p1
DEPENDS="openssl zlib"
CONFIGURE_ARGS="--sysconfdir=/etc/ssh --with-privsep-user=sshd --with-privsep-path=/var/empty --without-pam --disable-strip --with-mantype=man"

port_install()
{
    make install-nokeys DESTDIR="$DEST"
}
