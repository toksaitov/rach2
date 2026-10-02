# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# NTBTLS, the TLS library GnuPG's dirmngr uses to reach a key server over
# hkps, built on Libgcrypt and Libksba, in place of GnuTLS, which is not
# ported.
VERSION=0.3.2
DEPENDS="libgpg-error libgcrypt libksba zlib"
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"
