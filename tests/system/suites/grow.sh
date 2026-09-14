#!/bin/sh
# grow.sh: a root grown in place by `growroot` on the recovery system.
#
# The disk is 16 GiB, sparse, or the installed machine's 1953525168
# sectors when RACH2_GROW_LENOVO is set, which is what the milestone 47
# record ran and takes most of an hour in e2fsck over the grown volume;
# it has a 1 GiB ext2 volume in entry 1 at sector 2048 and nothing after
# it. Each row boots recovery on a qcow2 overlay of that disk, so the base
# is never changed and `qemu-img map` afterwards lists exactly the sectors
# the row wrote. The rows:
#
# 1. `growroot MBRID=...,1` at the memory the grow was measured to need:
#    the entry rewritten to the disk's end, the table read again, e2fsck,
#    resize2fs; every sector written lies inside the set `growset` computed
#    from the volume before the grow; e2fsck on the grown volume finds it
#    clean; the volume mounted and a file written near its end, read back,
#    and the volume clean again.
# 2. The same grow killed inside resize2fs, then run again: the checker
#    repairs what the kill left and the resizer finishes.
# 3. A disk whose entry already reaches the end, a grow stopped between
#    the entry and the resize: `growroot` finds nothing to rewrite and does
#    the rest.
# 4. A machine below the memory the grow needs: refused by name before
#    anything is read. No machine this system boots on has less than the
#    7 MiB the grow needs, so the row runs growroot built for the host,
#    where `/proc/meminfo` does not exist and the memory reads as none.
#
# Usage: grow.sh RKERNEL RIMAGE HARNESS MKMBR GROWSET DIR
set -u
RKERNEL=$1; RIMAGE=$2; HARNESS=$3; MKMBR=$4; GROWSET=$5; DIR=$6
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "grow-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"
. "$REPO/tools/setup/e2fsprogs_path.sh"

DISK_SECTORS=$([ -n "${RACH2_GROW_LENOVO:-}" ] && echo 1953525168 || echo 33554432)
START=2048
SIZE=2097152
END=$((DISK_SECTORS - START))
ID=47470001
# The smallest machine on which the recovery system boots and the grow of
# a 1 GiB root to this disk finishes, measured on 2026-09-07 with every
# power of two down to 16 MiB; 8 MiB does not boot. Its MemTotal, 7232 kB,
# is what utils/growroot.c refuses below.
MEM=16
# The kill lands inside resize2fs: the 1 GiB check takes a few seconds,
# the grow minutes on the Lenovo's disk and seconds on the small one.
KILL_AFTER=$([ -n "${RACH2_GROW_LENOVO:-}" ] && echo 25 || echo 6)

BASE="$DIR/base.raw"
FULL="$DIR/full.raw"
rm -f "$BASE" "$FULL"
dd if=/dev/zero of="$BASE" bs=512 count=1 seek=$((DISK_SECTORS - 1)) 2>/dev/null || fail "the base disk"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -E offset=$((START * 512)) "$BASE" $((SIZE / 2048))M > /dev/null 2>&1 || fail "the volume"
"$MKMBR" --lba --id $ID "$BASE" $START:$SIZE:83 > /dev/null || fail "mkmbr"
# The head of the volume, for growset: the superblock, the descriptors and
# the resize inode's blocks all lie in the first group.
dd if="$BASE" of="$DIR/head.vol" bs=1048576 skip=1 count=128 2>/dev/null || fail "the volume's head"
"$GROWSET" --lba $START "$DIR/head.vol" $END > "$DIR/set.txt" || fail "growset"
grep '^total\|^blocks' "$DIR/set.txt" | sed 's/^/grow-test: set: /'

# The backing file's path is kept relative to the overlay, so it is given
# absolute.
overlay() {
    rm -f "$1"
    qemu-img create -q -f qcow2 -o cluster_size=4096 -b "$(cd "$(dirname "$2")" && pwd)/$(basename "$2")" \
        -F raw "$1" || fail "the overlay $1"
}
# A recovery boot on overlay $1 with $2 MiB, the log $3, then the steps.
# Recovery gives a root shell at the console with nothing typed, so what is
# waited for is its own first line and then the prompt.
boot() {
    over=$1; mib=$2; log=$3; shift 3
    "$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-m --qemu-arg="$mib" \
        --drive "file=$over,format=qcow2,if=ide,index=0,media=disk,discard=unmap,detect-zeroes=unmap" \
        --timeout 7200 --steptmo 3000 --log "$log" --ok "ROWDONE" \
        "expect=recovery image" "expect=~ #" "$@" \
        > /dev/null 2>&1
}
GROWN="growroot: result: /dev/disk0p1 grown to $END sectors"
FSCK="send=/sbin/e2fsck -fn /dev/disk0p1 > /dev/null 2>&1; echo FSCK\"\"\$?"

# ---- 1: the grow, its writes inside the set, and a file near the end ----
overlay "$DIR/one.qcow2" "$BASE"
boot "$DIR/one.qcow2" $MEM "$DIR/one.log" \
    "send=growroot MBRID=$ID,1" "expect=$GROWN" \
    "$FSCK" "expect=FSCK0" "send=echo ROW\"\"DONE" "expect=ROWDONE" || fail "the grow"
grep -aq "growroot: entry: /dev/disk0p1 from $SIZE to $END sectors" "$DIR/one.log" || fail "the entry's record"
qemu-img map --output=json "$DIR/one.qcow2" > "$DIR/one.map" || fail "qemu-img map"
"$GROWSET" --lba $START --map "$DIR/one.map" "$DIR/head.vol" $END > "$DIR/one.cover" \
    || { grep '^outside\|^coverage' "$DIR/one.cover" | head -5 >&2; fail "a sector written outside the grow set"; }
grep '^coverage' "$DIR/one.cover" | sed 's/^/grow-test: /'
echo "grow-test: a 1 GiB root grown to $END sectors at $MEM MiB, every write inside the grow set, e2fsck clean: OK"
# The grown volume: mounted, a file placed in its last group, read back.
LAST=$(sed -n 's/^blocks \([0-9]*\) .*/\1/p' "$DIR/set.txt")
boot "$DIR/one.qcow2" $MEM "$DIR/end.log" \
    "send=mkdir -p /mnt && mount -t ext2 /dev/disk0p1 /mnt && echo MOUNT\"\"OK" "expect=MOUNTOK" \
    "send=dd if=/dev/urandom of=/tmp/end bs=4096 count=8 2>/dev/null; cksum < /tmp/end" \
    "send=dd if=/tmp/end of=/mnt/end.bin bs=4096 seek=$((LAST - 40)) conv=notrunc 2>/dev/null; sync; echo WRITE\"\"OK" "expect=WRITEOK" \
    "send=dd if=/mnt/end.bin bs=4096 skip=$((LAST - 40)) count=8 2>/dev/null | cksum" \
    "send=umount /mnt && echo UMOUNT\"\"OK" "expect=UMOUNTOK" \
    "$FSCK" "expect=FSCK0" "send=echo ROW\"\"DONE" "expect=ROWDONE" || fail "the file near the end"
# Two checksums of 32768 bytes each, written and read, and they must be the
# same one. Counting the distinct lines alone would pass a run that produced
# one line and lost the other, so the count of lines is checked as well.
SUMS=$(tr -d '\r' < "$DIR/end.log" | grep -ac '^[0-9]* 32768$')
UNIQ=$(tr -d '\r' < "$DIR/end.log" | grep -a '^[0-9]* 32768$' | sort -u | wc -l | tr -d ' ')
[ "$SUMS" = "2" ] && [ "$UNIQ" = "1" ] || fail "the file near the end did not read back ($SUMS lines, $UNIQ distinct)"
echo "grow-test: a file written in the grown volume's last group reads back and the volume is clean: OK"

# ---- 2: killed inside resize2fs, repaired and finished by the next run ----
overlay "$DIR/two.qcow2" "$BASE"
boot "$DIR/two.qcow2" $MEM "$DIR/two.log" \
    "send=growroot MBRID=$ID,1 > /tmp/g.log 2>&1 & sleep $KILL_AFTER; pidof resize2fs > /tmp/r.pid; kill \$!; killall resize2fs; sleep 2; cat /tmp/g.log; test -s /tmp/r.pid && echo WAS\"\"RESIZING; pidof resize2fs || echo NO\"\"RESIZER; echo KILL\"\"DONE" \
    "expect=WASRESIZING" "expect=NORESIZER" "expect=KILLDONE" \
    "send=growroot MBRID=$ID,1" "expect=$GROWN" \
    "$FSCK" "expect=FSCK0" "send=echo ROW\"\"DONE" "expect=ROWDONE" || fail "the grow killed and run again"
grep -aq 'growroot: grow: resize2fs /dev/disk0p1' "$DIR/two.log" || fail "the first run reached resize2fs"
grep -aq "growroot: entry: /dev/disk0p1 already fills the $END sectors after it" "$DIR/two.log" \
    || fail "the second run's entry record"
echo "grow-test: a grow killed inside resize2fs is repaired and finished by running growroot again: OK"

# ---- 3: the entry already at the end, the grow stopped before the resize ----
cp -c "$BASE" "$FULL" 2>/dev/null || cp "$BASE" "$FULL"
"$MKMBR" --lba --id $ID "$FULL" $START:$END:83 > /dev/null || fail "mkmbr full"
overlay "$DIR/three.qcow2" "$FULL"
boot "$DIR/three.qcow2" $MEM "$DIR/three.log" \
    "send=growroot MBRID=$ID,1" "expect=$GROWN" "send=echo ROW\"\"DONE" "expect=ROWDONE" \
    || fail "the grow after the entry"
grep -aq "growroot: entry: /dev/disk0p1 already fills the $END sectors after it" "$DIR/three.log" \
    || fail "the entry found already at its size"
echo "grow-test: a grow interrupted between the entry and the resize is finished by running again: OK"

# ---- 4: too little memory, refused by name before anything is read ----
# The refusal is proved on the host, where the memory can be dictated: the
# program is built with a requirement no machine meets, so what it reads
# from `/proc/meminfo` decides nothing. A host that has that file and one
# that does not both take this path.
cc -DGROWROOT_MEMORY_MIB=1048576u -o "$DIR/growroot-host" "$REPO/utils/growroot.c" \
    > "$DIR/growroot-host.log" 2>&1 || fail "growroot on the host"
"$DIR/growroot-host" MBRID=$ID,1 > "$DIR/four.log" 2>&1; rc=$?
[ "$rc" -eq 1 ] || fail "the refusal's status ($rc)"
grep -q '^growroot: refused: [0-9]* MiB of memory; growing a root needs 1048576 MiB$' "$DIR/four.log" \
    || fail "the refusal by name"
echo "grow-test: a machine without the memory is refused by name and nothing is read: OK"
rm -f "$DIR"/*.qcow2 "$BASE" "$FULL"
