# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Git, with curl for the HTTP transports, OpenSSL, zlib, Perl, and PCRE2 for
# git grep -P, and without Tcl/Tk, gettext, Python or expat: there is no
# gitk, git gui, translation, or push over WebDAV. Git is built by its own
# Makefile in the source tree, set by config.mak. Its Rust parts are still
# optional in 2.55 and are left out, since no Rust compiler for the target is
# built here; Git uses its C versions of them. HOST_CPU is the target's
# processor, which the Makefile would otherwise take from the build host, and
# curl's flags come from the root's curl-config. The dashed forms of the
# built-in commands are left out and the other links to git are symbolic.
# The manual pages come from Git's prebuilt tarball, since making them needs
# asciidoc.
VERSION=2.55.0
DEPENDS="curl openssl zlib perl pcre2"

port_build()
{
    cd "$SRC" || exit
    cat >config.mak <<EOF
CC = $TARGET-gcc
AR = $TARGET-ar
prefix = /usr
HOST_CPU = ${TARGET%%-*}
CURL_CONFIG = $ROOT/usr/bin/curl-config
NO_RUST = YesPlease
NO_EXPAT = YesPlease
NO_TCLTK = YesPlease
NO_GETTEXT = YesPlease
NO_PYTHON = YesPlease
USE_LIBPCRE2 = YesPlease
PERL_PATH = /usr/bin/perl
SHELL_PATH = /bin/sh
SKIP_DASHED_BUILT_INS = YesPlease
INSTALL_SYMLINKS = YesPlease
EOF
    make
}

port_install()
{
    cd "$SRC" || exit
    make install DESTDIR="$DEST"
    mkdir -p "$DEST/usr/share/man"
    tar -xf "$("$DIR/fetch.sh" --path git-manpages)" -C "$DEST/usr/share/man"
}
