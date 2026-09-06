#!/bin/sh
# queue.sh: the AHCI disks driven by their interrupts.
#
# One disk throttled by the emulator and one at full speed: a reader of the
# fast one finishes while a reader of the slow one is still waiting, and
# the controller's vector counts the completions. Then the fallback root
# on the disk, whose faults poll the port, beside a raw reader that keeps a
# command queued on the same port.
#
# Usage: queue.sh KERNEL IMAGE HARNESS MKMBR DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; DIR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "queue-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"

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
