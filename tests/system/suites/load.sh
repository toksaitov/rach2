#!/usr/bin/env sh
#
# load.sh: the machine under load and the longest delays it saw.
#
# `loadprobe` runs its load for twenty seconds, prints the four delays
# `/proc/stat` keeps as its longest, and requires what it counts: three lines
# read and no request quarantined by its deadline. Its writers have a disk
# with a FAT32 partition, under the lag device so that their writes go
# through the request layer whose delays are recorded; its pinger has the
# harness's network card; and its reader has the console, where the harness
# types each line once the reader has said it took the last. The lag's
# deadline counts the flush after each write, so that disk keeps the guest's
# flushes from the host (cache=unsafe), as tests/system/Readme.md says.
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
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk,cache=unsafe" \
    --append "rach2.rescue rach2.blocklag=1" \
    --timeout 120 --steptmo 60 --ok "LOADPROBE OK" --never "LOADPROBE FAIL" --log "$DISK.log" \
    "expect=Type 'help'" "send=exec /loadprobe" \
    "send=one" "expect=LOADPROBE read" "send=two" "expect=LOADPROBE read" \
    "send=three" "expect=LOADPROBE read" "send=four" "expect=LOADPROBE read" \
    "send=five" "expect=LOADPROBE read" "send=six" "expect=LOADPROBE read" \
    "expect=LOADPROBE" > /dev/null 2>&1; then
    grep -a 'LOADPROBE timer' "$DISK.log" | tail -1 | tr -d '\r' | sed 's/^/load-test: /'
    echo "load-test: twenty seconds of spinning, writing, pinging and typing: every line read, the counters printed, nothing quarantined: OK"
else
    grep -a 'LOADPROBE' "$DISK.log" | tr -d '\r' >&2 || true
    fail "the load boot"
fi
# The first writer alone, without its syncs, and the reader (`Wnr`), on a
# disk whose driver cannot sleep: `rach2.atapio` keeps the ATA driver on the
# data port, whose transfers run with interrupts off, so the writer is inside
# a system call from one write to the next. A timer interrupt that lands
# inside the call requests a switch, and the call's exit takes it, so the
# reader gets its lines and the probe's own sleep ends.
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue rach2.atapio" \
    --timeout 120 --steptmo 60 --ok "LOADPROBE OK" --never "LOADPROBE FAIL" --log "$DISK.polled.log" \
    "expect=Type 'help'" "send=exec /loadprobe Wnr" \
    "send=one" "expect=LOADPROBE read" "send=two" "expect=LOADPROBE read" \
    "send=three" "expect=LOADPROBE read" "send=four" "expect=LOADPROBE read" \
    "send=five" "expect=LOADPROBE read" "send=six" "expect=LOADPROBE read" \
    "expect=LOADPROBE" > /dev/null 2>&1; then
    grep -a 'LOADPROBE timer' "$DISK.polled.log" | tail -1 | tr -d '\r' | sed 's/^/load-test: polled: /'
    echo "load-test: a writer always inside a system call on the polled disk gives way at each tick: OK"
else
    grep -a 'LOADPROBE' "$DISK.polled.log" | tr -d '\r' >&2 || true
    fail "the polled writer's boot"
fi
echo "load-test: PASS"
