#!/bin/sh
# datapart.sh: shrink the Lenovo's installed root, GPT entry 3, to 64 GiB and
# make the sectors after it, up to entry 5, the data partition, entry 6, with
# the ext2 file system Rach2's recovery makes there (rach2-install --data).
#
# Usage: sh datapart.sh MKE2FS.CONF
#
# Runs as root on Debian booted from entry 5, with entry 3 unmounted.
# MKE2FS.CONF is vendor/e2fsprogs/mke2fs.conf, the defaults Rach2's mke2fs is
# built with. Each step is skipped when an earlier run did it, so a run that
# stopped may be run again. The table is written once, from a dump of the
# table as found, which keeps the disk's GUID and every entry's GUID, type
# and name; only entry 3's size changes and entry 6 is added. Exits 0 when
# every check at the end holds.
set -eu

DISK=/dev/sda
DISK_GUID=1ACEF00D-0F1E-4D2C-8B3A-C0FFEE000001
P3_UUID=45444342-4746-4948-4849-4A4B4C4D4E4F
P5_UUID=F640712E-B236-4C5A-B7D8-1B149D598EC0
LINUX_FS=0FC63DAF-8483-4772-8E79-3D69D8477DE4
P3_START=266240
# Entry 3 as found reaches the sector before entry 5; 64 GiB is 134217728
# sectors, and entry 6 takes the rest, 136.76 GiB.
P3_OLD=421021696
P3_NEW=134217728
P6_START=134483968
P6_SIZE=286803968
P5_START=421287936
STATE=/root/datapart

say() { echo "datapart: $*"; }
die() { echo "datapart: $*" >&2; exit 1; }

[ $# -eq 1 ] && [ -f "$1" ] || die "usage: sh datapart.sh MKE2FS.CONF"
conf=$1
mkdir -p "$STATE"

# entry N FIELD: FIELD (start, size, uuid, name) of entry N in the table.
entry() {
    sfdisk -d "$DISK" | sed -n "s|^$DISK$1 : ||p" | tr ',' '\n' |
        sed -n "s/^ *$2= *//p" | tr -d '"'
}
# kernel N FIELD: the kernel's start or size of entry N, or nothing.
kernel() { cat "/sys/class/block/sda$1/$2" 2>/dev/null || true; }

# ---- the disk is the Lenovo's, as this script expects it ----
[ "$(sfdisk -d "$DISK" | sed -n 's/^label-id: //p')" = "$DISK_GUID" ] ||
    die "$DISK does not have the Lenovo's disk GUID $DISK_GUID"
[ "$(entry 3 start)" = "$P3_START" ] && [ "$(entry 3 uuid)" = "$P3_UUID" ] ||
    die "entry 3 is not the installed root at sector $P3_START"
[ "$(entry 5 start)" = "$P5_START" ] && [ "$(entry 5 uuid)" = "$P5_UUID" ] ||
    die "entry 5 is not Debian's partition at sector $P5_START"
[ "$(findmnt -n -o SOURCE /)" = "${DISK}5" ] || die "/ is not ${DISK}5"
! findmnt -S "${DISK}3" > /dev/null || die "${DISK}3 is mounted"
! findmnt -S "${DISK}6" > /dev/null || die "${DISK}6 is mounted"
p3=$(entry 3 size)
case "$p3" in
    "$P3_OLD" | "$P3_NEW") ;;
    *) die "entry 3 has $p3 sectors, neither $P3_OLD as found nor $P3_NEW as made" ;;
esac
[ -f "$STATE/before.sfdisk" ] || {
    [ "$p3" = "$P3_OLD" ] || die "no dump of the table as found in $STATE"
    sfdisk -d "$DISK" > "$STATE/before.sfdisk"
    say "the table as found is in $STATE/before.sfdisk"
}

# ---- the root's file system, shrunk to 64 GiB ----
bsize=$(dumpe2fs -h "${DISK}3" 2>/dev/null | sed -n 's/^Block size: *//p')
blocks=$(dumpe2fs -h "${DISK}3" 2>/dev/null | sed -n 's/^Block count: *//p')
[ -n "$bsize" ] && [ -n "$blocks" ] || die "${DISK}3 has no ext2 file system"
want=$((P3_NEW * 512 / bsize))
if [ "$blocks" -gt "$want" ]; then
    say "checking ${DISK}3"
    rc=0
    e2fsck -f -p "${DISK}3" || rc=$?
    [ "$rc" -le 1 ] || die "e2fsck -f -p ${DISK}3 exited $rc; nothing was shrunk"
    say "the least ${DISK}3 can shrink to: $(resize2fs -P "${DISK}3" 2>/dev/null | sed -n 's/.*: //p') blocks of $bsize"
    say "shrinking ${DISK}3 from $blocks to $want blocks of $bsize"
    resize2fs "${DISK}3" "$want" || die "resize2fs ${DISK}3 $want failed"
    blocks=$(dumpe2fs -h "${DISK}3" 2>/dev/null | sed -n 's/^Block count: *//p')
fi
[ "$blocks" -le "$want" ] || die "${DISK}3 still has $blocks blocks"
say "result: ${DISK}3 has $blocks blocks of $bsize, $((blocks * bsize / 512)) sectors"

# ---- the table: entry 3 shortened and entry 6 added ----
if [ "$p3" = "$P3_OLD" ]; then
    [ -z "$(entry 6 start)" ] || die "entry 6 is used already"
    uuid=$(cat /proc/sys/kernel/random/uuid)
    sed "s|^\(${DISK}3 : .*size= *\)$P3_OLD,|\1$P3_NEW,|" "$STATE/before.sfdisk" > "$STATE/after.sfdisk"
    printf '%s6 : start=%s, size=%s, type=%s, uuid=%s, name="Rach2 data"\n' \
        "$DISK" "$P6_START" "$P6_SIZE" "$LINUX_FS" "$uuid" >> "$STATE/after.sfdisk"
    [ "$(diff "$STATE/before.sfdisk" "$STATE/after.sfdisk" | grep -c '^[<>]')" = 3 ] ||
        die "the new table differs from the old in more than entry 3's size and entry 6"
    say "writing the table: entry 3 to $P3_NEW sectors, entry 6 from $P6_START, $P6_SIZE sectors"
    sfdisk --no-reread --wipe never --wipe-partitions never "$DISK" < "$STATE/after.sfdisk" ||
        die "sfdisk did not write the table"
fi
[ "$(entry 3 size)" = "$P3_NEW" ] || die "entry 3 is not $P3_NEW sectors in the table"
[ "$(entry 6 start)" = "$P6_START" ] && [ "$(entry 6 size)" = "$P6_SIZE" ] &&
    [ "$(entry 6 name)" = "Rach2 data" ] || die "entry 6 is not the data partition in the table"
# The kernel learns the new sizes from sfdisk; partx is the second way.
[ "$(kernel 3 size)" = "$P3_NEW" ] || partx -u --nr 3 "$DISK" || true
[ -n "$(kernel 6 start)" ] || partx -a --nr 6 "$DISK" || true
[ "$(kernel 3 size)" = "$P3_NEW" ] || die "the kernel has entry 3 at $(kernel 3 size) sectors"
[ "$(kernel 6 start)" = "$P6_START" ] && [ "$(kernel 6 size)" = "$P6_SIZE" ] ||
    die "the kernel does not have entry 6 as the table does"
say "result: entry 3 and entry 6 in the table and in the kernel"

# ---- the data partition's file system ----
if [ "$(dd if="${DISK}6" bs=1 skip=1080 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 53ef ] &&
    e2fsck -fn "${DISK}6" > /dev/null 2>&1; then
    say "${DISK}6 has its file system already"
else
    say "making the data partition's file system on ${DISK}6"
    MKE2FS_CONFIG=$conf mke2fs -q -F -t ext2 -T huge -L "Rach2 data" "${DISK}6" ||
        die "mke2fs ${DISK}6 failed"
fi
sync

# ---- the checks ----
e2fsck -fn "${DISK}3" > "$STATE/e2fsck-p3.txt" 2>&1 || die "e2fsck -fn ${DISK}3 found faults: $STATE/e2fsck-p3.txt"
e2fsck -fn "${DISK}6" > "$STATE/e2fsck-p6.txt" 2>&1 || die "e2fsck -fn ${DISK}6 found faults: $STATE/e2fsck-p6.txt"
sfdisk -d "$DISK" > "$STATE/now.sfdisk"
dumpe2fs -h "${DISK}6" > "$STATE/p6.dumpe2fs" 2>/dev/null
say "result: both file systems check clean"
say "result: ${DISK}6 $(sed -n 's/^Filesystem features: *//p' "$STATE/p6.dumpe2fs"); $(sed -n 's/^Block count: *//p' "$STATE/p6.dumpe2fs") blocks, $(sed -n 's/^Inode count: *//p' "$STATE/p6.dumpe2fs") inodes, label $(sed -n 's/^Filesystem volume name: *//p' "$STATE/p6.dumpe2fs")"
diff "$STATE/before.sfdisk" "$STATE/now.sfdisk" || true
say "done"
