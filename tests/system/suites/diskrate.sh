#!/bin/sh
# diskrate.sh: the rate of the storage paths, each timed in the machine and
# held under a bound: the raw device read, a file written and read back on
# exFAT and on FAT32, two hundred small files, and a raw read through a
# pipe into a hash. The boot asks for the whole-disk nodes, which the
# ordinary kernel publishes only then and lets only the recovery build
# write, so the raw device is read and not written here. The bounds are
# three times what this host measured on 2026-09-09, so a path that falls
# back to a page a request, a walk of the chain per block or a write
# through per cluster fails by a wide margin while an ordinary slow day
# passes. One boot of the os-real
# image with three IDE disks: a raw one, an exFAT volume and a FAT32 one.
#
# Usage: diskrate.sh KERNEL OSREAL HARNESS DIR PASSWORD MKEXFATFIXTURE MKMBR
set -u
KERNEL=$1; OSREAL=$2; HARNESS=$3; DIR=$4; PASSWORD=$5; MKEXFAT=$6; MKMBR=$7

fail() { echo "diskrate-test: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR"
RAW="$DIR/raw.img"; EX="$DIR/exfat.img"; FAT="$DIR/fat.img"; LOG="$DIR/diskrate.log"
dd if=/dev/zero of="$RAW" bs=1048576 seek=255 count=1 2>/dev/null || fail "the raw disk"
"$MKEXFAT" --make "$DIR/exfat.vol" --sectors 522240 --offset 2048 >/dev/null || fail "the exFAT volume"
{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat "$DIR/exfat.vol"; } > "$EX" || fail "the exFAT disk"
rm -f "$DIR/exfat.vol"
"$MKMBR" --lba "$EX" 2048:522240:07 >/dev/null || fail "the exFAT disk's table"
dd if=/dev/zero of="$FAT" bs=1048576 seek=255 count=1 2>/dev/null || fail "the FAT32 disk"
"$MKMBR" --lba "$FAT" 2048:520192:0c >/dev/null || fail "the FAT32 disk's table"
mformat -i "$FAT@@1048576" -F -T 520192 -v RATEFAT :: || fail "mformat"

# Each row is one typed line that prints `T_NAME SECONDS`; the machine's
# clock is read before and after, so the time is the machine's own.
timed() { # timed NAME COMMAND
    # shellcheck disable=SC2016
    printf 's=$(date +%%s); %s; echo T_%s $(( $(date +%%s) - s ))' "$2" "$1"
}
# shellcheck disable=SC2016
set -- \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" "expect=# " \
    "send=$(timed rawread 'dd if=/dev/disk0 of=/dev/null bs=1M count=128')" "expect=T_rawread" \
    "send=$(timed pipehash 'dd if=/dev/disk0 bs=1M count=64 | sha256sum > /dev/null')" "expect=T_pipehash" \
    "send=mkdir -p /e /f && mount -t exfat /dev/disk1p1 /e && mount -t vfat /dev/disk2p1 /f && echo MOUNT\"\"ED" "expect=MOUNTED" \
    "send=$(timed exwrite 'dd if=/dev/zero of=/e/big bs=1M count=64 2>/dev/null; sync')" "expect=T_exwrite" \
    "send=$(timed exread 'dd if=/e/big of=/dev/null bs=1M 2>/dev/null; dd if=/e/big of=/dev/null bs=1M 2>/dev/null')" "expect=T_exread" \
    "send=$(timed fatwrite 'dd if=/dev/zero of=/f/big bs=1M count=64 2>/dev/null; sync')" "expect=T_fatwrite" \
    "send=$(timed fatread 'dd if=/f/big of=/dev/null bs=1M 2>/dev/null; dd if=/f/big of=/dev/null bs=1M 2>/dev/null')" "expect=T_fatread" \
    "send=$(timed exsmall 'dd if=/dev/zero of=/e/small bs=4k count=4096; sync')" "expect=T_exsmall" \
    "send=$(timed fatsmall 'dd if=/dev/zero of=/f/small bs=4k count=4096; sync')" "expect=T_fatsmall" \
    "send=$(timed exfiles 'i=0; while [ $i -lt 200 ]; do dd if=/dev/disk0 of=/e/small-$i bs=4k skip=$i count=8; i=$((i+1)); done; sync')" "expect=T_exfiles" \
    "send=umount /e && umount /f && echo ALL\"\"DONE" "expect=ALLDONE"
"$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 --append "rach2.diskdiag" \
    --drive "file=$RAW,format=raw,if=ide,index=0,media=disk" \
    --drive "file=$EX,format=raw,if=ide,index=1,media=disk" \
    --drive "file=$FAT,format=raw,if=ide,index=2,media=disk" \
    --timeout 900 --steptmo 300 --ok "ALLDONE" --log "$LOG" "$@" > /dev/null 2>&1 \
    || fail "the boot did not reach its end; see $LOG"

# A row's time against its bound; the second read of a file is served
# from the cache, so a read row times a cold read and a warm one together.
rc=0
row() { # row NAME BOUND TEXT
    t=$(tr -d '\r' < "$LOG" | grep -a "^T_$1 [0-9]" | tail -1 | awk '{print $2}')
    [ -n "$t" ] || fail "no time for $1 in $LOG"
    if [ "$t" -le "$2" ]; then
        echo "diskrate-test: $3: $t s, under $2 s: OK"
    else
        echo "diskrate-test: FAIL: $3: $t s, over $2 s" >&2
        rc=1
    fi
}
row rawread 30 "128 MiB read from the raw device"
row pipehash 30 "64 MiB read raw through a pipe into sha256sum"
row exwrite 20 "64 MiB written into a file on exFAT and synced"
row exread 20 "the exFAT file read twice, cold and from the cache"
row fatwrite 40 "64 MiB written into a file on FAT32 and synced"
row fatread 20 "the FAT32 file read twice, cold and from the cache"
row exsmall 60 "16 MiB written on exFAT in 4 KiB calls"
row fatsmall 60 "16 MiB written on FAT32 in 4 KiB calls"
row exfiles 30 "two hundred 32 KiB files written on exFAT"
[ "$rc" -eq 0 ] || fail "a row is over its bound"
echo "diskrate-test: PASS"
