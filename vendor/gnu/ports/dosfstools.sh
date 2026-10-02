# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# dosfstools, the FAT tools Debian has: mkfs.fat, fsck.fat and fatlabel,
# with the names mkfs.vfat, mkdosfs, fsck.vfat, dosfsck and dosfslabel that
# Debian links to them. They answer by the path before BusyBox's mkdosfs
# and mkfs.vfat, as the other ports' programs do, and BusyBox has no
# checker of FAT.
VERSION=4.2
CONFIGURE_ARGS="--enable-compat-symlinks"
