#!/usr/bin/env sh
#
# usbdisk.sh: a flash drive as a disk, from its table to its removal.
#
# Three sticks are built on the host with the tools the tree has: one with an
# MBR holding a FAT32 and an exFAT volume, one with a GUID table holding the
# same two with its first used entry at index 6, so that a reader which took
# the first four entries of the array would find nothing, and one that is an
# exFAT volume from sector zero with no table at all. Each is attached to the
# emulated controller as `usb-storage`, beside the keyboard and the mouse the
# USB suite uses, and the probe on the machine reads what the kernel made of
# it.
#
# The pull and the replug go through the emulator's monitor while the probe
# holds a file open on the mounted volume, because a stick pulled out from
# under a mount is the case the removal code exists for and cannot be
# arranged from inside the machine.
#
# Usage: usbdisk.sh KERNEL IMAGE HARNESS DIR OSREAL PASSWORD MKEXFATFIXTURE
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DIR=$4; OSREAL=$5; PASSWORD=$6; MKFIX=$7
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MKMBR="$REPO/build/tools/image/mkmbr"
MKGPT="$REPO/build/tools/image/mkgpt"

fail() { echo "usbdisk-test: FAIL: $1" >&2; exit 1; }
note() { echo "usbdisk-test: $1"; }

mkdir -p "$DIR" || fail "mkdir"
MBR="$DIR/stick-mbr.img"
GPT="$DIR/stick-gpt.img"
SUPER="$DIR/stick-super.img"

# ---- the fixtures ----
# The FAT32 volume: 48 MiB at sector 2048, with a text file and a pattern
# file the hold boot reads in a loop. Forty-eight and not thirty-two because
# a FAT32 volume must have at least 65525 clusters, and mformat will write
# one with fewer; the kernel mounts such a volume, but BusyBox's blkid judges
# by the cluster count, takes it for FAT16 and reads the label from the
# FAT16 field, which is empty. The exFAT volume: the fixture's own 16 MiB,
# placed at sector 100352.
"$MKFIX" "$DIR/exfat.vol" > /dev/null || fail "the exFAT volume"
echo "hello from the stick" > "$DIR/hello.txt"
awk 'BEGIN { for (i = 0; i < 128; i++) for (j = 0; j < 128; j++) printf "%03d.", i }' \
    > "$DIR/pattern.bin" || fail "the pattern"

fill_volumes() {
    # -T: the volume is the partition's 98304 sectors and not the rest of
    # the image; the driver refuses a volume that claims more than the
    # partition has.
    mformat -i "$1"@@1048576 -F -T 98304 -v STICKFAT :: || fail "mformat $1"
    mcopy -i "$1"@@1048576 "$DIR/hello.txt" ::/hello.txt || fail "mcopy hello $1"
    mcopy -i "$1"@@1048576 "$DIR/pattern.bin" ::/pattern.bin || fail "mcopy pattern $1"
    dd if="$DIR/exfat.vol" of="$1" bs=512 seek=100352 conv=notrunc 2>/dev/null || fail "placing exFAT on $1"
    "$MKFIX" --check "$1" 100352 > /dev/null 2>&1 || fail "the exFAT volume on $1 does not check"
}

dd if=/dev/zero of="$MBR" bs=1048576 count=80 2>/dev/null || fail "the MBR image"
"$MKMBR" --lba "$MBR" 2048:98304:0c 100352:32768:07 > /dev/null || fail "mkmbr"
fill_volumes "$MBR"

dd if=/dev/zero of="$GPT" bs=1048576 count=80 2>/dev/null || fail "the GUID image"
"$MKGPT" "$GPT" 2048:98304:fat@6 100352:32768:data@9 > /dev/null || fail "mkgpt"
fill_volumes "$GPT"

cp "$DIR/exfat.vol" "$SUPER" || fail "the superfloppy"

# A stick on the controller beside the keyboard and the mouse, with the 8042
# left in place: nothing here types, and the keyboard is there so that a
# stick brought up beside a device that is polled every tick costs it
# nothing, which the interrupt suite already checks the other way round.
run() {
    stick=$1; shift
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
        --drive "if=none,id=stick,format=raw,file=$stick" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
        "$@"
}
# ---- 1: the MBR stick is named, sized, a block device, and both volumes
#         mount; the table reads back through /dev/usb0 ----
if run "$MBR" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    "expect=Type 'help'" "send=exec /usbdiskprobe report" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe volumes" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "an MBR stick is named, sized, and both its volumes mount: OK"
else
    fail "the MBR stick boot"
fi

# ---- 2: the GUID stick, whose first used entry is the seventh ----
if run "$GPT" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    "expect=Type 'help'" "send=exec /usbdiskprobe report" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe volumes" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "a GUID stick with its first used entry at index 6 mounts both volumes: OK"
else
    fail "the GUID stick boot"
fi

# ---- 3: a stick with no table mounts through its whole node ----
LOG="$DIR/super.log"
if run "$SUPER" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe super" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "a stick with no partition table mounts as one volume: OK"
else
    fail "the superfloppy boot"
fi
if grep -aq "usbdisk: usb0: one volume" "$LOG"; then
    note "and the kernel recorded it as one volume: OK"
else
    fail "the superfloppy was not recorded as one volume"
fi

# ---- 4: a write reaches the stick: written, synced, unmounted, read back
#         after a remount, and seen by mtools on the host afterwards ----
cp "$MBR" "$DIR/stick-write.img" || fail "the write image"
LOG="$DIR/write.log"
if run "$DIR/stick-write.img" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe write" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "a file written to the stick comes back after a remount: OK"
else
    fail "the write boot"
fi
if mtype -i "$DIR/stick-write.img"@@1048576 ::/written.txt 2>/dev/null | grep -q "a stick takes a write"; then
    note "and mtools reads it off the image on the host: OK"
else
    fail "the written file is not on the image"
fi

# ---- 5: the stick pulled out from under a mount with a file open on it:
#         the read in flight answers EIO, umount2 is EBUSY while the file is
#         open and succeeds once it is closed; then the same stick plugged
#         back in mounts again with nothing inherited, and the log shows the
#         first one reclaimed once the mount went. The emulator drops the
#         medium with the device it was under, so the replug makes the
#         medium again before the device. ----
cp "$MBR" "$DIR/stick-pull.img" || fail "the pull image"
LOG="$DIR/pull.log"
if run "$DIR/stick-pull.img" --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe hold" "expect=USBDISKPROBE HOLDING" \
    wait=1 "usb=del:st0" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe free" "expect=USBDISKPROBE FREE 2" \
    "expect=Type 'help'" \
    "drive=add:if=none,id=again,format=raw,file=$DIR/stick-pull.img" \
    "usb=add:usb-storage,bus=xhci.0,drive=again,id=st1" "expect=usbdisk: usb0: mbr" \
    "send=exec /usbdiskprobe replug" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "a pulled stick answers EIO, unmounts after its file closes, and mounts again when plugged back: OK"
else
    fail "the pull and replug boot"
fi
if grep -aq "usbdisk: usb0: reclaimed" "$LOG"; then
    note "the pulled stick was reclaimed once nothing held it: OK"
else
    fail "the pulled stick was never reclaimed"
fi

# ---- 6: a stick plugged in and pulled out without being opened, ten times:
#         reclaimed at once each time, and every name free afterwards ----
LOG="$DIR/churn.log"
set -- "expect=Type 'help'"
i=0
while [ $i -lt 10 ]; do
    set -- "$@" "drive=add:if=none,id=medium$i,format=raw,file=$MBR" \
        "usb=add:usb-storage,bus=xhci.0,drive=medium$i,id=st$i" "expect=usbdisk: usb0: mbr" \
        "usb=del:st$i" "expect=usbdisk: usb0: reclaimed"
    i=$((i + 1))
done
set -- "$@" "send=exec /usbdiskprobe free" "expect=USBDISKPROBE FREE 2"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --timeout 240 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" "$@" > /dev/null 2>&1; then
    note "ten plugs and pulls of a stick nobody opened leave every name free: OK"
else
    fail "the churn boot"
fi
if [ "$(grep -ac 'usbdisk: usb0: reclaimed' "$LOG")" = 10 ]; then
    note "and each of the ten was reclaimed: OK"
else
    fail "the ten sticks were not each reclaimed"
fi

# ---- 7: BusyBox finds the volume by its label, copies a file off it and
#         unmounts it, with no change to BusyBox ----
LOG="$DIR/busybox.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --drive "if=none,id=stick,format=raw,file=$MBR" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
    --timeout 240 --steptmo 60 --ok "BBDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=blkid; echo BLK\"\"DONE" \
    "expect=BLKDONE" "send=findfs LABEL=STICKFAT; findfs LABEL=FIXTURE; echo FIND\"\"DONE" \
    "expect=FINDDONE" "send=mkdir -p /mnt && mount -t vfat LABEL=STICKFAT /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=cp /mnt/hello.txt /tmp/h && cat /tmp/h; echo COPY\"\"DONE" \
    "expect=COPYDONE" "send=umount /mnt && echo BB\"\"DONE" \
    "expect=BBDONE" > /dev/null 2>&1; then
    note "BusyBox mounts the stick by its label, copies from it and unmounts: OK"
else
    fail "the BusyBox boot"
fi
if grep -a '^/dev/usb0p1' "$LOG" | grep -aq 'STICKFAT'; then
    note "findfs named /dev/usb0p1 for the label: OK"
else
    fail "findfs did not name the partition for its label"
fi
if grep -a '^/dev/usb0p2' "$LOG" | grep -aq 'FIXTURE'; then
    note "blkid found the exFAT volume on /dev/usb0p2 too: OK"
else
    fail "blkid did not find the exFAT volume, whose prober reads part of a sector"
fi
if grep -aq 'hello from the stick' "$LOG"; then
    note "the copied file has the stick's text: OK"
else
    fail "the copied file did not read"
fi

# ---- 8: throughput, recorded and not judged: a 16 MiB read of /dev/usb0
#         timed by the machine's own clock, with the controller's messages
#         on, which is the ordinary machine. On the timer's pump alone the
#         same read takes two minutes, three ticks a page, and that number
#         is in the milestone's notes rather than in a boot the core tier
#         waits for. ----
LOG="$DIR/throughput.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$MBR" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
    --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe throughput" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "$(grep -a 'USBDISKPROBE THROUGHPUT' "$LOG" | head -1 | tr -d '\r')"
else
    fail "the throughput boot"
fi
if [ "$(grep -ac 'usbdisk: recovery' "$LOG")" = 0 ]; then
    note "and no command needed recovery on the way: OK"
else
    fail "a command needed recovery during the read"
fi

note "PASS"
