#!/usr/bin/env sh
#
# mkrootdisk.sh: the ext2 root disks of the bootline and servicelog cases.
#
# Each disk has one MBR partition, type 0x83, at sector 2048, with a MiB of
# zeros after it, which holds a volume the host's mke2fs makes from a tree.
# The volume has the profile mkext2.sh gives a root, so the kernel's driver
# mounts it writable, as it mounts a machine's.
#
#   loop        8 MiB, whose /sbin/init is a symbolic link to itself, so
#               that the kernel's lookup of init answers ELOOP
#   servicelog  16 MiB, with init-shell as /sbin/init, BusyBox as
#               /bin/busybox and /bin/sh, and an /etc/profile that prints
#               the line `servicelog: shell up`, which a boot waits for,
#               since the prompt alone is too short to tell from the
#               kernel's lines
#
# Usage: mkrootdisk.sh loop DISK MKMBR
#        mkrootdisk.sh servicelog DISK MKMBR INIT BUSYBOX
#
set -eu

[ "$#" -ge 3 ] || { echo "mkrootdisk: usage: mkrootdisk.sh loop|servicelog DISK MKMBR [INIT BUSYBOX]" >&2; exit 2; }
KIND=$1
DISK=$2
MKMBR=$3
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TREE="$DISK.tree"
VOL="$DISK.vol"

. "$REPO/tools/setup/e2fsprogs_path.sh"

trap 'rm -rf "$TREE" "$VOL" "$DISK.tmp"' EXIT
rm -rf "$TREE" "$VOL" "$DISK.tmp"
case $KIND in
    loop)
        MIB=8
        mkdir -p "$TREE/sbin"
        ln -s init "$TREE/sbin/init"
        ;;
    servicelog)
        [ "$#" -eq 5 ] || { echo "mkrootdisk: servicelog needs INIT BUSYBOX" >&2; exit 2; }
        MIB=16
        mkdir -p "$TREE/sbin" "$TREE/bin" "$TREE/etc" "$TREE/var/log"
        cp "$4" "$TREE/sbin/init"
        cp "$5" "$TREE/bin/busybox"
        cp "$5" "$TREE/bin/sh"
        chmod 755 "$TREE/sbin/init" "$TREE/bin/busybox" "$TREE/bin/sh"
        printf 'export PATH=/bin\necho servicelog: shell up\n' > "$TREE/etc/profile"
        ;;
    *)
        echo "mkrootdisk: unknown disk: $KIND" >&2
        exit 2
        ;;
esac

PROFILE="sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"
: > "$VOL"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$PROFILE" -d "$TREE" "$VOL" "${MIB}M"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$VOL"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$DISK.tmp"
"$MKMBR" --lba "$DISK.tmp" "2048:$((MIB * 2048)):83" > /dev/null
mv "$DISK.tmp" "$DISK"
