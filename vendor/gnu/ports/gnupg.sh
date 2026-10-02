# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GnuPG: gpg, gpg-agent, gpgsm, dirmngr with NTBTLS for hkps, keyboxd with
# SQLite, scdaemon and the tools. There is no LDAP and no TPM2 daemon, since
# OpenLDAP and the TSS library are not ported. The manual pages are made from
# the Texinfo manual by yat2m, which GnuPG 2.5 takes from libgpg-error; it
# is compiled for the build host from libgpg-error's source and given to make.
VERSION=2.5.24
DEPENDS="libgpg-error libgcrypt libassuan libksba npth ntbtls pinentry sqlite readline zlib bzip2"
CONFIGURE_ARGS="--disable-nls --disable-ldap --disable-gnutls --disable-tpm2d"
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"

port_build()
{
    set -- "$OUT"/src/libgpg-error-*/doc/yat2m.c
    cc -O2 -o yat2m-build "$1"
    configure_autoconf
    make YAT2M="$WORK/yat2m-build"
}

port_install()
{
    make install DESTDIR="$DEST" YAT2M="$WORK/yat2m-build"
}
