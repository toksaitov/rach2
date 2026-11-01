# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# socat, the relay between two byte streams of almost any kind, which Claude
# Code's sandbox runs to carry its network proxy into the sandbox, with
# OpenSSL for its TLS addresses, and its filan and procan. Debian's patches
# make it build with OpenSSL 4, whose ASN1_STRING is opaque, and correct its
# page, which the release carries. socat is GPL version 2 only and Readline is
# GPL version 3, so there is no READLINE address, as on Debian. configure
# runs test programs, which a cross build cannot run; CACHE gives glibc's
# answers on x86: C99's snprintf, printf's Z modifier, and the bit offsets of
# the masks CRDLY, TABDLY and CSIZE in a termios flag word. make install
# copies the release's three scripts from the build directory, so
# port_install puts them there first.
VERSION=1.8.1.3
DEBIAN=socat-debian
DEPENDS=openssl
CACHE="ac_cv_have_c99_snprintf=yes ac_cv_have_z_modifier=yes sc_cv_sys_crdly_shift=9 sc_cv_sys_tabdly_shift=11 sc_cv_sys_csize_shift=4"
CONFIGURE_ARGS="--disable-readline --disable-libwrap"

port_install()
{
    cp "$SRC"/socat-broker.sh "$SRC"/socat-chain.sh "$SRC"/socat-mux.sh .
    make install DESTDIR="$DEST"
}
