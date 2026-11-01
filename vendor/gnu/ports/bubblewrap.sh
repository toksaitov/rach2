# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# bubblewrap, whose bwrap runs a program in new namespaces as a sandbox, as
# Claude Code's sandboxed shell runs its commands. It is not set-uid, so it
# works only where the kernel lets an ordinary user make a user namespace,
# and it has no SELinux and no completions, for which the root has no bash
# or zsh framework. Its page is DocBook, which needs xsltproc and DocBook's
# stylesheets, which the build host lacks; port_install takes the page that
# Debian's package of the release made from the same bwrap.xml. Its utils.h
# defines bool, which C23 makes a keyword, so it is compiled as C17.
VERSION=0.8.0
DEPENDS=libcap
CONFIGURE_ARGS="--disable-man --disable-selinux --with-priv-mode=none --with-bash-completion-dir=no --with-zsh-completion-dir=no"

port_build()
{
    configure_autoconf CFLAGS="-O2 -std=gnu17"
    make
}

port_install()
{
    make install DESTDIR="$DEST"
    rm -rf deb
    mkdir deb
    (
        cd deb || exit
        "$TARGET-ar" x "$("$DIR/fetch.sh" --path bubblewrap-man)" data.tar.xz
        tar -xJf data.tar.xz ./usr/share/man/man1/bwrap.1.gz
    )
    mkdir -p "$DEST/usr/share/man/man1"
    gzip -dc deb/usr/share/man/man1/bwrap.1.gz >"$DEST/usr/share/man/man1/bwrap.1"
}
