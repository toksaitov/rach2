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

# The rows are typed as short lines, since the console takes a line of
# bounded length: two functions are defined on the machine first. `t NAME
# COMMAND` runs COMMAND through `eval` and prints `T_NAME SECONDS`, the
# machine's clock read before and after, or `T_NAME FAIL` when it failed;
# `moved BYTES COMMAND...` runs a dd that must report BYTES copied.
# shellcheck disable=SC2016
DEFS='t() { s=$(date +%s); if eval "$2"; then echo T_$1 $(( $(date +%s) - s )); else echo T_$1 FAIL; fi; }; moved() { n=$1; shift; "$@" 2>/tmp/dd.err && grep -q "^$n bytes" /tmp/dd.err; }; echo DEFS""DONE'
# shellcheck disable=SC2016
set -- \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" "expect=# " \
    "send=$DEFS" "expect=DEFSDONE" \
    "send=t rawread 'moved 134217728 dd if=/dev/disk0 of=/dev/null bs=1M count=128'" "expect=T_rawread" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=mkdir -p /e /f && mount -t exfat /dev/disk1p1 /e && mount -t vfat /dev/disk2p1 /f && echo MOUNT\"\"ED" "expect=MOUNTED" \
    "send=t exwrite 'moved 67108864 dd if=/dev/zero of=/e/big bs=1M count=64 && sync'" "expect=T_exwrite" \
    "send=t exread 'moved 67108864 dd if=/e/big of=/dev/null bs=1M && moved 67108864 dd if=/e/big of=/dev/null bs=1M'" "expect=T_exread" \
    "send=t fatwrite 'moved 67108864 dd if=/dev/zero of=/f/big bs=1M count=64 && sync'" "expect=T_fatwrite" \
    "send=t fatread 'moved 67108864 dd if=/f/big of=/dev/null bs=1M && moved 67108864 dd if=/f/big of=/dev/null bs=1M'" "expect=T_fatread" \
    "send=t exsmall 'moved 16777216 dd if=/dev/zero of=/e/small bs=4k count=4096 && sync'" "expect=T_exsmall" \
    "send=t fatsmall 'moved 16777216 dd if=/dev/zero of=/f/small bs=4k count=4096 && sync'" "expect=T_fatsmall" \
    "send=t exfiles 'i=0; n=0; while [ \$i -lt 200 ]; do dd if=/dev/disk0 of=/e/small-\$i bs=4k skip=\$i count=8 2>/dev/null && n=\$((n+1)); i=\$((i+1)); done; sync; [ \$n = 200 ] && [ \$(stat -c %s /e/small-199) = 32768 ]'" "expect=T_exfiles" \
    "send=umount /e && umount /f && echo ALL\"\"DONE" "expect=ALLDONE"
"$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 --append "rach2.diskdiag" \
    --drive "file=$RAW,format=raw,if=ide,index=0,media=disk" \
    --drive "file=$EX,format=raw,if=ide,index=1,media=disk" \
    --drive "file=$FAT,format=raw,if=ide,index=2,media=disk" \
    --timeout 900 --steptmo 300 --ok "ALLDONE" --log "$LOG" "$@" > /dev/null 2>&1 \
    || fail "the boot did not reach its end; see $LOG"

# row NAME BOUND TEXT: the row's time against its bound, the least of its
# runs where a row runs more than once; a row whose command failed has no
# time and fails. The second read of a file is served from the cache, so a
# read row times a cold read and a warm one together. The pipe row runs
# three times because the emulator, not the kernel, sometimes runs the
# reader's copies thirty times slower for a whole run (entry 26 of
# milestone 50), and one run of the three at the usual rate is the kernel's.
rc=0
row() {
    t=$(tr -d '\r' < "$LOG" | grep -a "^T_$1 [0-9]" | awk '{print $2}' | sort -n | head -1)
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
