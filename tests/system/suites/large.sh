#!/bin/sh
# large.sh: files past 4 GiB, and the formats' limits refused by name.
#
# One AHCI disk with three partitions: a 6 GiB ext2 volume, a 6 GiB exFAT
# volume the host's own formatter made, and a small FAT32 one. On each of
# the first two, bigfileprobe writes a file, stats it, seeks it, reads it
# at its 4 GiB line or its middle and near its end, shares it between a
# second open and a duplicate, truncates it and appends to it; a second
# boot reads the whole of it back; on ext2 a mebibyte written astride the
# 4 GiB line of a sparse file is read back with its hole zero; and the
# host's checker finds each volume consistent. Then a write past each
# format's limit: 2 TiB less a block on ext2, the reach of its pointers
# and its block count, 4 GiB on FAT32, and the seek's own reach on exFAT,
# each refused with EFBIG and named in the kernel log.
#
# The file is 256 MiB unless RACH2_LARGE_MIB says otherwise; a file past
# 4 GiB, such as 5120, takes hours at the kernel's rates. The disk is sparse
# and stays so.
#
# Usage: large.sh KERNEL IMAGE HARNESS MKMBR DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; DIR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "large-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"
DISK="$DIR/large.disk"
MIB=${RACH2_LARGE_MIB:-256}
# The partitions, in sectors: each volume 6 GiB, the FAT32 one 64 MiB.
P1=2048;        P1N=12582912
P2=12584960;    P2N=12582912
P3=25167872;    P3N=131072
END=$((P3 + P3N + 2048))

. "$REPO/tools/setup/e2fsprogs_path.sh"
# The exFAT volume is made and checked with what the host has: macOS attaches
# the raw image and works on its second slice, Linux works on a copy of the
# slice with exfatprogs and writes it back.
if command -v hdiutil >/dev/null 2>&1 && command -v newfs_exfat >/dev/null 2>&1; then
    EXFAT_HOST=mac
elif command -v mkfs.exfat >/dev/null 2>&1 && command -v fsck.exfat >/dev/null 2>&1; then
    EXFAT_HOST=linux
else
    fail "the exFAT volume needs hdiutil and newfs_exfat, or mkfs.exfat and fsck.exfat"
fi

rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=512 count=1 seek=$((END - 1)) 2>/dev/null || fail "the disk"
"$MKMBR" --lba --id 1a26e001 "$DISK" $P1:$P1N:83 $P2:$P2N:07 $P3:$P3N:0c >/dev/null || fail "mkmbr"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -E offset=$((P1 * 512)) "$DISK" $((P1N / 2048))M > /dev/null 2>&1 || fail "the ext2 volume"
if [ "$EXFAT_HOST" = mac ]; then
    DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$DISK" 2>/dev/null \
        | awk 'NR == 1 {print $1}')
    [ -n "$DEV" ] || fail "attaching the disk for newfs_exfat"
    newfs_exfat -v LARGEX "${DEV}s2" > /dev/null 2>&1 || { hdiutil detach "$DEV" > /dev/null 2>&1; fail "newfs_exfat"; }
    hdiutil detach "$DEV" > /dev/null 2>&1 || fail "detaching the disk"
else
    rm -f "$DISK.slice"
    truncate -s $((P2N * 512)) "$DISK.slice" || fail "the exFAT slice"
    mkfs.exfat -L LARGEX "$DISK.slice" > /dev/null 2>&1 || fail "mkfs.exfat"
    dd if="$DISK.slice" of="$DISK" bs=1048576 seek=$((P2 / 2048)) conv=notrunc,sparse 2>/dev/null \
        || fail "writing the exFAT slice"
    rm -f "$DISK.slice"
fi
mformat -i "$DISK"@@$((P3 * 512)) -F -T $P3N -v LARGEFAT :: || fail "mformat"

# run LOG ARGS STEP...: one boot of the rescue shell on the disk that runs
# the probe once with ARGS, logged to LOG. The STEPs come before the wait for
# the probe's verdict and keep the harness's step clock alive while a
# gibibyte at a time goes by. The status is the harness's.
run() {
    log=$1; args=$2; shift 2
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=none,id=drv0" \
        --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
        --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
        --append "rach2.rescue" --timeout 7200 --steptmo 1800 --log "$log" \
        --ok "BIGFILE OK" --never "BIGFILE FAIL" \
        "expect=Type 'help'" "send=exec /bigfileprobe $args" "$@" "expect=BIGFILE" > /dev/null 2>&1
}
# The steps that keep the harness's step clock alive, one per gibibyte
# written; the script's own arguments are in their variables by now.
set --
i=1
while [ "$i" -le $((MIB / 1024)) ]; do set -- "$@" "expect=bigfile: written $i GiB"; i=$((i + 1)); done
for fs in ext2:/dev/disk0p1 exfat:/dev/disk0p2; do
    type=${fs%%:*}; node=${fs#*:}
    run "$DIR/$type-write.log" "write $node $type $MIB" "$@" || fail "the $MIB MiB write on $type"
    grep -a 'bigfile: rate' "$DIR/$type-write.log" | tail -1 | sed "s/^/large-test: $type /"
    run "$DIR/$type-verify.log" "verify $node $type $MIB" || fail "the read back after a remount on $type"
    echo "large-test: a $MIB MiB file on $type written, sought, shared, truncated, appended and read back: OK"
done
run "$DIR/ext2-sparse.log" "sparse /dev/disk0p1 ext2 0" || fail "the sparse file astride 4 GiB on ext2"
echo "large-test: a mebibyte astride the 4 GiB line of a sparse ext2 file reads back with its hole zero: OK"
e2fsck -fn "$DISK?offset=$((P1 * 512))" > "$DIR/ext2.fsck" 2>&1 || fail "e2fsck on the ext2 volume after the file"
if [ "$EXFAT_HOST" = mac ]; then
    DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$DISK" 2>/dev/null \
        | awk 'NR == 1 {print $1}')
    [ -n "$DEV" ] || fail "attaching the disk for fsck_exfat"
    fsck_exfat -n "${DEV}s2" > "$DIR/exfat.fsck" 2>&1; rc=$?
    hdiutil detach "$DEV" > /dev/null 2>&1
else
    dd if="$DISK" of="$DISK.slice" bs=1048576 skip=$((P2 / 2048)) count=$((P2N / 2048)) 2>/dev/null \
        || fail "reading the exFAT slice"
    fsck.exfat -n "$DISK.slice" > "$DIR/exfat.fsck" 2>&1; rc=$?
    rm -f "$DISK.slice"
fi
[ "$rc" -eq 0 ] || { cat "$DIR/exfat.fsck" >&2; fail "fsck_exfat on the exFAT volume after the file"; }
echo "large-test: the host's checkers find both volumes consistent: OK"

# The limits. The ext2 one is the file limit for 4096-byte blocks less a
# block, so the write lands past it; FAT32's is 4 GiB; exFAT's is the
# largest offset a seek can name.
run "$DIR/ext2-limit.log" "limit /dev/disk0p1 ext2 2196875759616" || fail "the ext2 limit"
grep -aq 'ext2: write: offset 2095103 MiB is past the file limit of 2095103 MiB for 4096-byte blocks' \
    "$DIR/ext2-limit.log" || fail "the ext2 refusal by name"
run "$DIR/vfat-limit.log" "limit /dev/disk0p3 vfat 4294967295" || fail "the FAT32 limit"
grep -aq 'vfat: write: offset 4095 MiB is past the format.s limit of 4 GiB' "$DIR/vfat-limit.log" \
    || fail "the FAT32 refusal by name"
run "$DIR/exfat-limit.log" "limit /dev/disk0p2 exfat 9223372036854775807" || fail "the exFAT limit"
grep -aq "exfat: write: offset past the format's limit" "$DIR/exfat-limit.log" \
    || fail "the exFAT refusal by name"
echo "large-test: a write past each format's limit is refused by name: OK"
rm -f "$DISK"
