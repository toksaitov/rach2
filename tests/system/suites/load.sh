#!/usr/bin/env sh
#
# load.sh: the machine under load and the longest delays it saw.
#
# A disk with a FAT32 partition for the writers, under the lag device so
# their writes go through the request layer, whose delays are the ones
# recorded, the harness's own card for the pinger, and the harness typing a line at the console every two seconds for the
# reader; `loadprobe` runs the load for twenty seconds and prints the four
# delays `/proc/stat` keeps as its longest, which this suite requires within
# their bounds and prints, since the numbers are the record.
#
# Usage: load.sh KERNEL IMAGE HARNESS DISK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "load-test: FAIL: $1" >&2; exit 1; }

# 14 MiB of FAT32 at LBA 2048.
dd if=/dev/zero of="$DISK" bs=1048576 count=16 2>/dev/null || fail "the disk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:28672:0c >/dev/null || fail "mkmbr"
mformat -i "$DISK"@@1048576 -F -T 28672 -v LOAD :: || fail "mformat"

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue rach2.blocklag=1" \
    --timeout 120 --steptmo 60 --ok "LOADPROBE OK" --never "LOADPROBE FAIL" --log "$DISK.log" \
    "expect=Type 'help'" "send=exec /loadprobe" \
    wait=2 "send=one" wait=2 "send=two" wait=2 "send=three" wait=2 "send=four" \
    wait=2 "send=five" wait=2 "send=six" \
    "expect=LOADPROBE" > /dev/null 2>&1; then
    grep -a 'LOADPROBE timer' "$DISK.log" | tail -1 | tr -d '\r' | sed 's/^/load-test: /'
    echo "load-test: twenty seconds of spinning, writing, pinging and typing, with every delay within its bound: OK"
else
    grep -a 'LOADPROBE' "$DISK.log" | tr -d '\r' >&2 || true
    fail "the load boot"
fi
echo "load-test: PASS"
