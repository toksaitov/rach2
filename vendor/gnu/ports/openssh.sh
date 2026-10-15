# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# OpenSSH: the server, the client, ssh-keygen, sftp-server and the rest, with
# OpenSSL for its cryptography and zlib for compression. The configuration is
# in /etc/ssh, the server's privilege-separated child runs as the user sshd in
# /var/empty, and there is no PAM. The child that talks to a client before it
# authenticates runs in the rlimit sandbox, whose limits of 0 leave it no
# descriptor to open, no process to fork and no file to grow, as this kernel
# enforces them (prlimit64(2)). The recipe asks for that sandbox in place of
# the seccomp sandbox configure chooses for a Linux target, and configure
# accepts it only when a test program shows that poll and select work under a
# descriptor limit of 0, which a cross build cannot run; they do on this
# kernel, as limitprobe checks, and port_build gives configure that answer. A
# session's path, which a command run without a login shell keeps, is the one
# /etc/profile gives a login, with what is installed under /usr/local first,
# as Debian's server gives it. The programs are installed without the build
# host's strip and without host keys, which the target makes itself.
VERSION=10.5p1
DEPENDS="openssl zlib"
CONFIGURE_ARGS="--sysconfdir=/etc/ssh --with-privsep-user=sshd --with-privsep-path=/var/empty --without-pam --with-sandbox=rlimit --disable-strip --with-mantype=man --with-default-path=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin --with-superuser-path=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin"

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
