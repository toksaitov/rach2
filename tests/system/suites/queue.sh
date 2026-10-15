#!/bin/sh
# queue.sh: the AHCI disks driven by their interrupts.
#
# One disk throttled by the emulator and one at full speed: a reader of the
# fast one finishes while a reader of the slow one is still waiting, and
# the controller's vector counts the completions. Then the fallback root
# on the disk, whose faults poll the port, beside a raw reader that keeps a
# command queued on the same port. Then a disk throttled below one request
# in a deadline period, whose late answers must be taken as the answers; the
# deadline's boundary on the lag device; a late completion that frees a
# quarantined slot; and a reader that finds the request pool full.
#
# Usage: queue.sh KERNEL IMAGE HARNESS MKMBR DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; DIR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "queue-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"

# fat_disk FILE: an 8 MiB disk with one FAT32 partition from sector 2048.
fat_disk() {
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1048576 count=8 2>/dev/null || fail "the disk $1"
    "$MKMBR" --lba "$1" 2048:14336:0c >/dev/null || fail "mkmbr $1"
    mformat -i "$1"@@1048576 -F -T 14336 :: || fail "mformat $1"
}
fat_disk "$DIR/slow.disk"
fat_disk "$DIR/fast.disk"
# The slow disk is behind the emulator's throttle filter, 256 KiB a second
# after a burst of one second's worth, which the filter cannot be told to
# make smaller; past the burst a page-sized request takes 16 ms and a raw
# window's call 128 ms.
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-object --qemu-arg=throttle-group,id=tg0,x-bps-total=262144 \
    --qemu-arg=-blockdev --qemu-arg="driver=file,filename=$DIR/slow.disk,node-name=slowfile" \
    --qemu-arg=-blockdev --qemu-arg=driver=raw,file=slowfile,node-name=slowraw \
    --qemu-arg=-blockdev --qemu-arg=driver=throttle,throttle-group=tg0,file=slowraw,node-name=slow \
    --drive "file=$DIR/fast.disk,format=raw,if=none,id=drv1" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=slow,bus=ahci0.0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv1,bus=ahci0.1 \
    --append "rach2.rescue" --timeout 120 --steptmo 60 --log "$DIR/slow.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe slow" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the slow and the fast disk"
grep -aq 'ahci: interrupts: messages on vector' "$DIR/slow.log" || fail "the driver did not switch to its interrupts"
echo "queue-test: a reader of the fast disk is not held behind the slow one: $(grep -a 'AHCIPROBE READ' "$DIR/slow.log" | tr -d '\r' | tr '\n' ' ')OK"

# The fallback root on AHCI, with a FAT32 second partition the raw reader reads.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DIR/root.disk" 64 48 \
    >/dev/null 2>&1 || fail "mkdisk"
"$MKMBR" --lba "$DIR/root.disk" 2048:98304:7f 100352:28672:0c >/dev/null || fail "mkmbr root"
mformat -i "$DIR/root.disk"@@51380224 -F -T 28672 :: || fail "mformat root"
"$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/root.disk,format=raw,if=none,id=drv0" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --append "rach2.root=disk rach2.rescue" --timeout 180 --steptmo 120 --log "$DIR/fault.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe fault" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the fallback root beside the queue"
grep -aq 'BLOCKPROBE OK' "$DIR/fault.log" || fail "blockprobe on the root"
echo "queue-test: the root's faults share the port with the queue: $(grep -a 'AHCIPROBE READ' "$DIR/fault.log" | tr -d '\r' | tr '\n' ' ')OK"

# A request answered after its deadline is the answer, not a failure. The
# disk is throttled to 512 KiB a second, so a 1 MiB request, the cache's read
# run and the most an ext2 write's end gathers, takes two seconds, and
# `rach2.blockdeadline=1` sets the period to one second: the request passes
# its deadline, is answered inside the four periods a request is waited for,
# and the mount keeps writing. The throttle is set against the largest
# request: at 16 KiB a second a run of 1 MiB would outlast the four periods
# and the disk would be declared stalled. The volume is ext2 and not FAT32
# because ext2 writes a file back in runs, where FAT32 writes each 4 KiB
# block through as it is written, under the deadline. A write is the
# transfer and a flush in one request, and QEMU makes the flush an
# fdatasync of the image, which on a loaded host can take longer than the
# request's four periods; the image's node ignores the guest's flushes
# (cache.no-flush), so the throttle alone sets how long a request takes.
rm -f "$DIR/late.vol"; : > "$DIR/late.vol"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    "$DIR/late.vol" 6M > /dev/null 2>&1 || fail "the late volume"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$DIR/late.vol"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$DIR/late.disk"
"$MKMBR" --lba "$DIR/late.disk" 2048:12288:83 > /dev/null || fail "the late disk's table"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-object --qemu-arg=throttle-group,id=tg1,x-bps-total=524288 \
    --qemu-arg=-blockdev --qemu-arg="driver=file,filename=$DIR/late.disk,node-name=latefile,cache.no-flush=on" \
    --qemu-arg=-blockdev --qemu-arg=driver=raw,file=latefile,node-name=lateraw \
    --qemu-arg=-blockdev --qemu-arg=driver=throttle,throttle-group=tg1,file=lateraw,node-name=late \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=late,bus=ahci0.0 \
    --append "rach2.rescue rach2.blockdeadline=1" --timeout 240 --steptmo 180 --log "$DIR/late.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe late" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the late answers: $(grep -a 'AHCIPROBE FAIL\|read-only\|stalled' "$DIR/late.log" | head -2 | tr -d '\r' | tr '\n' ' ')"
grep -aq 'block: request overdue' "$DIR/late.log" || fail "no request passed its deadline, so the row shows nothing"
grep -aq 'block: late answer: taken' "$DIR/late.log" || fail "no late answer was taken"
if grep -aq 'vfs: read-only' "$DIR/late.log"; then fail "the mount went read-only"; fi
echo "queue-test: a request answered after its deadline is taken as the answer and the mount keeps writing: $(grep -a -A1 'AHCIPROBE WRITE' "$DIR/late.log" | tr -d '\r' | tr '\n' ' ')OK"

# The deadline's boundary, on the lag device, which answers a request an
# exact number of ticks after it was made, where an emulator's throttle
# pays for a burst unevenly: under a period of a hundred ticks, set on the
# wrapper itself since it keeps its own, a lag of 75 ticks is answered
# inside the period and no request is overdue; a lag of 125 is overdue
# once and taken. The record names which. The deadline counts the IDE
# transfer under the lag too, and the flush after a write, which the disk
# keeps from the host (cache=unsafe), as the late row does.
fat_disk "$DIR/edge.disk"
edge_boot() { # edge_boot LAG OVERDUE
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DIR/edge.disk,format=raw,if=ide,index=0,media=disk,cache=unsafe" \
        --append "rach2.rescue rach2.blocklag=$1 rach2.lagdeadline=100 rach2.lagperiods=4" --timeout 120 --steptmo 60 \
        --log "$DIR/edge$1.log" --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
        "expect=Type 'help'" "send=exec /ahciprobe edge" "expect=AHCIPROBE OK" > /dev/null 2>&1 \
        || fail "the boundary at a lag of $1: $(grep -a 'AHCIPROBE FAIL' "$DIR/edge$1.log" | head -1 | tr -d '\r')"
    overdue=$(grep -ac 'block: request overdue' "$DIR/edge$1.log")
    [ "$overdue" = "$2" ] || fail "the boundary at a lag of $1: $overdue requests went overdue where $2 should have"
}
edge_boot 75 0
edge_boot 125 1
grep -aq 'block: late answer: taken after 1 deadlines' "$DIR/edge125.log" || fail "the overdue request was not taken as answered"
echo "queue-test: a request answered inside its period is not overdue and one answered a quarter past it is overdue once and taken: $(grep -a -A1 'AHCIPROBE EDGE' "$DIR/edge75.log" "$DIR/edge125.log" | tr -d '\r' | grep -o 'ms:$\|^[^:]*-[0-9]*$' | tr '\n' ' ')OK"

# A completion after quarantine frees the slot: one slot in the pool and
# the lag device answering 500 ticks late under a period of a hundred and
# four periods, so the one request is given up on with EIO at the fourth
# deadline and its slot quarantined; the answer arrives a second after and
# must free the slot: a request made then is the proof, since with one
# slot in the pool a slot not freed leaves nothing to take. The lag makes
# that request late as well, so it is taken and waits its periods, where
# a refused one would answer at once and the pool report itself stalled.
# Its disk keeps the guest's flushes from the host, as the boundary's does.
fat_disk "$DIR/quar.disk"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/quar.disk,format=raw,if=ide,index=0,media=disk,cache=unsafe" \
    --append "rach2.rescue rach2.blocklag=500 rach2.lagdeadline=100 rach2.lagperiods=4 rach2.blockpool=1" \
    --timeout 120 --steptmo 60 --log "$DIR/quar.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe quarantine" "expect=AHCIPROBE OK" > /dev/null 2>&1 \
    || fail "the completion after quarantine: $(grep -a 'AHCIPROBE FAIL\|stalled' "$DIR/quar.log" | head -1 | tr -d '\r')"
grep -aq 'quarantined' "$DIR/quar.log" || fail "no request was quarantined, so the row shows nothing"
grep -aq 'late completion: quarantined slot freed' "$DIR/quar.log" || fail "the late completion did not free the slot"
if grep -aq 'request pool stalled' "$DIR/quar.log"; then fail "a request after the late completion found the pool stalled"; fi
echo "queue-test: a request given up on is quarantined, its late completion frees the slot and the next request is admitted: $(grep -a -A1 'AHCIPROBE GAVEUP\|AHCIPROBE AFTER' "$DIR/quar.log" | tr -d '\r' | grep -v '^--' | tr '\n' ' ')OK"

# The pool exhausted: two slots, two disks throttled to 16 KiB a second
# each holding one for a 64 KiB read, and a reader of a third, unthrottled
# disk that finds the pool full, waits, and is served.
fat_disk "$DIR/pool1.disk"
fat_disk "$DIR/pool2.disk"
fat_disk "$DIR/pool3.disk"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-object --qemu-arg=throttle-group,id=tg4,x-bps-total=16384 \
    --qemu-arg=-object --qemu-arg=throttle-group,id=tg5,x-bps-total=16384 \
    --qemu-arg=-blockdev --qemu-arg="driver=file,filename=$DIR/pool1.disk,node-name=p1file" \
    --qemu-arg=-blockdev --qemu-arg=driver=raw,file=p1file,node-name=p1raw \
    --qemu-arg=-blockdev --qemu-arg=driver=throttle,throttle-group=tg4,file=p1raw,node-name=p1 \
    --qemu-arg=-blockdev --qemu-arg="driver=file,filename=$DIR/pool2.disk,node-name=p2file" \
    --qemu-arg=-blockdev --qemu-arg=driver=raw,file=p2file,node-name=p2raw \
    --qemu-arg=-blockdev --qemu-arg=driver=throttle,throttle-group=tg5,file=p2raw,node-name=p2 \
    --drive "file=$DIR/pool3.disk,format=raw,if=none,id=drv3" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=p1,bus=ahci0.0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=p2,bus=ahci0.1 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv3,bus=ahci0.2 \
    --append "rach2.rescue rach2.blockpool=2" --timeout 120 --steptmo 60 --log "$DIR/pool.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe pool" "expect=AHCIPROBE OK" > /dev/null 2>&1 \
    || fail "the pool exhausted: $(grep -a 'AHCIPROBE FAIL\|stalled' "$DIR/pool.log" | head -1 | tr -d '\r')"
grep -aq 'request pool: waiting for a slot: the pool is full' "$DIR/pool.log" || fail "no reader waited for the full pool, so the row shows nothing"
echo "queue-test: a reader that finds the pool full waits for a slot and is served: $(grep -a -A1 'AHCIPROBE WAITED' "$DIR/pool.log" | tr -d '\r' | tr '\n' ' ')OK"

