# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Libksba, the X.509 and CMS library of gpgsm, dirmngr and NTBTLS.
VERSION=1.8.1
DEPENDS=libgpg-error
CACHE="GPGRT_CONFIG=$ROOT/usr/bin/gpgrt-config"
