# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# OpenSSH: the server, the client, ssh-keygen, sftp-server and the rest, with
# OpenSSL for its cryptography and zlib for compression. The configuration is
# in /etc/ssh, the server's privilege-separated child runs as the user sshd
# in /var/empty, and there is no PAM. The child that talks to a client before
# it authenticates runs in the rlimit sandbox, with no descriptor to open and
# no process to fork; the sandbox's limit of 0 on file growth this kernel
# takes and does not enforce (prlimit64(2)). configure would choose the
# seccomp sandbox for a Linux target, which this kernel does not have, and
# takes the rlimit sandbox only when a test program shows that poll and
# select work under a descriptor limit of 0, which a cross build cannot run;
# they do on this kernel, as limitprobe checks, and port_build gives
# configure that answer. The programs are installed without the build host's strip and
# without host keys, which the target makes itself.
VERSION=10.5p1
DEPENDS="openssl zlib"
CONFIGURE_ARGS="--sysconfdir=/etc/ssh --with-privsep-user=sshd --with-privsep-path=/var/empty --without-pam --with-sandbox=rlimit --disable-strip --with-mantype=man"

port_build()
{
    # The first assignment in configure is its answer for a cross build.
    awk '!set && /select_works_with_rlimit=no/ { sub(/=no/, "=yes"); set = 1 } { print }' \
        "$SRC/configure" >"$SRC/configure.rach2"
    cat "$SRC/configure.rach2" >"$SRC/configure"
    if [ "$(grep -c 'select_works_with_rlimit=yes' "$SRC/configure")" -ne 2 ]; then
        echo "gnu: openssh: configure's answer for select under a descriptor limit is not where it was" >&2
        exit 1
    fi
    configure_autoconf
    make
}

port_install()
{
    make install-nokeys DESTDIR="$DEST"
}
