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
# Usage: usbdisk.sh KERNEL IMAGE HARNESS DIR OSREAL PASSWORD MKEXFATFIXTURE \
#        STICKIMAGE
set -u
if [ $# -ne 10 ]; then
    echo "usage: usbdisk.sh KERNEL IMAGE HARNESS DIR OSREAL PASSWORD MKEXFATFIXTURE STICKIMAGE RKERNEL RIMAGE" >&2
    exit 2
fi
KERNEL=$1; IMAGE=$2; HARNESS=$3; DIR=$4; OSREAL=$5; PASSWORD=$6; MKFIX=$7; STICKIMAGE=$8
RKERNEL=$9; RIMAGE=${10}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
sh "$REPO/tests/system/vfs_unmount_test.sh" || exit 1
sh "$REPO/tests/system/usbdisk_script_test.sh" || exit 1
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
#         mount; the table reads back through /dev/disk0 ----
if run "$MBR" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    "expect=Type 'help'" "send=exec /usbdiskprobe report" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe volumes" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "an MBR stick is named, sized, and both its volumes mount: OK"
else
    fail "the MBR stick boot"
fi

# ---- 2: the GUID stick, whose first used entry is the seventh, so its
#         nodes are p7 and p10: the table's own numbers ----
if run "$GPT" --timeout 120 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    "expect=Type 'help'" "send=exec /usbdiskprobe report" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe volumes /dev/disk0p7 /dev/disk0p10" \
    "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "a GUID stick with its first used entry at index 6 mounts both volumes as p7 and p10: OK"
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
if grep -aq "usbdisk: disk0: one volume" "$LOG"; then
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
for hold in holdnested holdnestedexfat; do
cp "$MBR" "$DIR/stick-pull.img" || fail "the pull image"
LOG="$DIR/pull-$hold.log"
if run "$DIR/stick-pull.img" --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" \
    --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe $hold" "expect=USBDISKPROBE HOLDING" \
    "usb=del:st0" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe free" "expect=USBDISKPROBE FREE 8" \
    "expect=Type 'help'" \
    "drive=add:if=none,id=again,format=raw,file=$DIR/stick-pull.img" \
    "usb=add:usb-storage,bus=xhci.0,drive=again,id=st1" "expect=usbdisk: disk0: mbr" \
    "send=exec /usbdiskprobe replug" "expect=USBDISKPROBE OK" \
    > /dev/null 2>&1; then
    note "$hold: a pulled stick releases child then parent and mounts again when plugged back: OK"
else
    fail "the pull and replug boot"
fi
if grep -aq "usbdisk: disk0: reclaimed" "$LOG"; then
    note "the pulled stick was reclaimed once nothing held it: OK"
else
    fail "the pulled stick was never reclaimed"
fi

done

# ---- 6: a stick plugged in and pulled out without being opened, ten times:
#         reclaimed at once each time, and every name free afterwards ----
LOG="$DIR/churn.log"
set -- "expect=Type 'help'"
i=0
while [ $i -lt 10 ]; do
    set -- "$@" "drive=add:if=none,id=medium$i,format=raw,file=$MBR" \
        "usb=add:usb-storage,bus=xhci.0,drive=medium$i,id=st$i" "expect=usbdisk: disk0: mbr" \
        "usb=del:st$i" "expect=usbdisk: disk0: reclaimed"
    i=$((i + 1))
done
set -- "$@" "send=exec /usbdiskprobe free" "expect=USBDISKPROBE FREE 8"
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
if [ "$(grep -ac 'usbdisk: disk0: reclaimed' "$LOG")" = 10 ]; then
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
    "expect=BLKDONE" "send=echo FOUND1=\$(findfs LABEL=STICKFAT); echo FOUND2=\$(findfs LABEL=FIXTURE); echo FIND\"\"DONE" \
    "expect=FINDDONE" "send=mkdir -p /mnt && mount -t vfat LABEL=STICKFAT /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=cp /mnt/hello.txt /tmp/h && cat /tmp/h; echo COPY\"\"DONE" \
    "expect=COPYDONE" "send=umount /mnt && echo BB\"\"DONE" \
    "expect=BBDONE" > /dev/null 2>&1; then
    note "BusyBox mounts the stick by its label, copies from it and unmounts: OK"
else
    fail "the BusyBox boot"
fi
if grep -aq '^FOUND1=/dev/disk0p1' "$LOG"; then
    note "findfs named /dev/disk0p1 for the FAT32 label: OK"
else
    fail "findfs did not name the FAT32 partition for its label"
fi
if grep -aq '^FOUND2=/dev/disk0p2' "$LOG"; then
    note "and /dev/disk0p2 for the exFAT label, whose prober reads part of a sector: OK"
else
    fail "findfs did not name the exFAT partition for its label"
fi
if grep -a '^/dev/disk0p1:' "$LOG" | grep -aq 'STICKFAT'; then
    note "blkid listed the FAT32 label: OK"
else
    fail "blkid did not list the FAT32 label"
fi
if grep -aq 'hello from the stick' "$LOG"; then
    note "the copied file has the stick's text: OK"
else
    fail "the copied file did not read"
fi

# ---- 8: throughput, recorded and not judged: a 16 MiB read of /dev/disk0
#         timed by the machine's own clock, with the controller's messages
#         on, which is the ordinary machine. On the timer's pump alone, with
#         neither messages nor the controller's line, the same read takes
#         two minutes, three ticks a page; the line, which `rach2.nomsi`
#         boots take, reads at the same rate as messages. ----
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

# ---- 9: the stick image booted by the firmware, with no kernel given to the
#         emulator: GRUB loads the metal kernel and its RAM root off the
#         stick, the stick comes up as the machine's own disk, and the
#         machine mounts the stick's second partition and reads a file from
#         it. The image is copied first, since the volume is written by the
#         mount's dirty flag and the built image is the record. ----
cp "$STICKIMAGE" "$DIR/stick-boot.img" || fail "the stick image"
LOG="$DIR/stickboot.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/stick-boot.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --timeout 240 --steptmo 120 --ok "STICKDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=cat /proc/usb | grep ^disk0; echo USB\"\"DONE" \
    "expect=USBDONE" "send=mkdir -p /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=ls /mnt | head -3; grep exfat /proc/mounts; echo LIST\"\"DONE" \
    "expect=LISTDONE" "send=umount /mnt && echo STICK\"\"DONE" \
    "expect=STICKDONE" > /dev/null 2>&1; then
    note "the machine booted from the stick image mounts the stick's second partition: OK"
else
    fail "the boot from the stick image"
fi
# The stick's size is the image's: the ISO half grows with the kernel, and
# the machine must name the whole of what it booted from.
STICK_SECTORS=$(($(wc -c < "$DIR/stick-boot.img") / 512))
if grep -aq "usbdisk: disk0: mbr: $((STICK_SECTORS / 2048)) MiB, 2 partitions" "$LOG"; then
    note "and the stick it booted from is its disk0 with two partitions: OK"
else
    fail "the booted machine did not name its own stick"
fi
if grep -aq '/dev/disk0p2 /mnt exfat' "$LOG"; then
    note "and /proc/mounts named the stick's second partition: OK"
else
    fail "/proc/mounts did not name the stick's partition"
fi

# ---- 10: three sticks at once, each mounted and read; then one of them,
#          the middle one, pulled while a reader keeps a command queued on
#          it: the read in flight answers EIO and the two beside it are
#          unharmed. The three are copies of the MBR stick. ----
for i in 0 1 2; do
    cp "$MBR" "$DIR/stick-three-$i.img" || fail "the three sticks' images"
done
three() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --drive "if=none,id=stick0,format=raw,file=$DIR/stick-three-0.img" \
        --drive "if=none,id=stick1,format=raw,file=$DIR/stick-three-1.img" \
        --drive "if=none,id=stick2,format=raw,file=$DIR/stick-three-2.img" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick0,id=st0 \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick1,id=st1 \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick2,id=st2 \
        "$@"
}
LOG="$DIR/three.log"
if three --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe three" "expect=USBDISKPROBE OK" \
    "expect=Type 'help'" "send=exec /usbdiskprobe pullone" "expect=USBDISKPROBE HOLDING" \
    "usb=del:st1" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "three sticks at once each mount, and the middle one pulled under a reader leaves the others unharmed: OK"
else
    fail "the three sticks boot"
fi
if grep -aq "usbdisk: disk1: reclaimed" "$LOG"; then
    note "and the pulled one was reclaimed by name: OK"
else
    fail "the pulled stick was not reclaimed as disk1"
fi

# ---- 11: a stick written raw from the recovery kernel: a sector pattern
#          through its whole node, read back there and on the host; then
#          4 MiB of a pattern written in 32 KiB pieces at 5 MiB, one dd a
#          piece since the recovery image cannot hold 4 MiB, and read back
#          byte for byte, each direction as 128 commands on the stick, which
#          /proc/disk counts, so a driver that fell back to a page a command
#          would show as 512 ----
cp "$MBR" "$DIR/stick-recovery.img" || fail "the recovery stick's image"
LOG="$DIR/recovery.log"
# shellcheck disable=SC2016
CMDS='grep "^disk-commands " /proc/disk | cut -d" " -f2'
if run "$DIR/stick-recovery.img" --kernel "$RKERNEL" --initrd "$RIMAGE" \
    --timeout 300 --steptmo 120 --ok "BIGDONE" --log "$LOG" \
    "expect=recovery image" "expect=~ #" \
    "send=yes RACH2RAW | head -c 4096 > /tmp/p; dd if=/tmp/p of=/dev/disk0 bs=512 seek=1000 2>&1; dd if=/dev/disk0 bs=512 skip=1000 count=8 2>/dev/null | cksum; cksum < /tmp/p; echo RAW\"\"DONE" \
    "expect=RAWDONE" \
    "send=yes RACH2-BIG-PATTERN-LINE | head -c 32768 > /tmp/base; p() { printf PIECE-%03d- \$1; dd if=/tmp/base bs=32758 count=1 2>/dev/null; }; c0=\$($CMDS); echo BIG\"\"START" "expect=BIGSTART" \
    "send=i=0; while [ \$i -lt 128 ]; do p \$i > /tmp/blk; dd if=/tmp/blk of=/dev/disk0 bs=32k seek=\$((160 + i)) count=1 2>/dev/null; i=\$((i + 1)); done; sync; c1=\$($CMDS); echo BIG\"\"WROTE" "expect=BIGWROTE" \
    "send=dd if=/dev/disk0 bs=32k skip=160 count=128 2>/dev/null | cksum; c2=\$($CMDS); i=0; while [ \$i -lt 128 ]; do p \$i; i=\$((i + 1)); done | cksum; echo BIGCMDS \$((c1-c0)) \$((c2-c1)); echo BIG\"\"DONE" \
    "expect=BIGDONE" > /dev/null 2>&1; then
    note "the recovery kernel wrote the stick raw: OK"
else
    fail "the recovery kernel's raw write of the stick"
fi
WANT=$(yes RACH2RAW | head -c 4096 | cksum | cut -d' ' -f1)
[ "$(grep -ac "^$WANT " "$LOG")" -ge 2 ] || fail "the pattern did not read back on the machine"
GOT=$(dd if="$DIR/stick-recovery.img" bs=512 skip=1000 count=8 2>/dev/null | cksum | cut -d' ' -f1)
[ "$GOT" = "$WANT" ] || fail "the pattern is not on the image"
note "and the host finds the pattern where it was written: OK"
# The pieces differ by their index, so a request landing at another
# piece's sectors shows.
yes RACH2-BIG-PATTERN-LINE | head -c 32768 > "$DIR/base"
BIGWANT=$(i=0; while [ $i -lt 128 ]; do printf "PIECE-%03d-" $i; head -c 32758 "$DIR/base"; i=$((i + 1)); done | cksum | cut -d' ' -f1)
[ "$(grep -ac "^$BIGWANT " "$LOG")" -ge 2 ] || fail "the 4 MiB pattern did not read back whole on the machine"
BIGGOT=$(dd if="$DIR/stick-recovery.img" bs=32k skip=160 count=128 2>/dev/null | cksum | cut -d' ' -f1)
[ "$BIGGOT" = "$BIGWANT" ] || fail "the 4 MiB pattern is not on the image"
BIGCMDS=$(tr -d '\r' < "$LOG" | grep -a "^BIGCMDS " | tail -1)
[ "$BIGCMDS" = "BIGCMDS 128 128" ] || fail "the 4 MiB write and read were not 128 commands each: $BIGCMDS"
note "4 MiB written raw in 32 KiB requests and read back byte for byte, 128 commands each way: OK"

# ---- 12: a stick past two terabytes: a sparse image with one partition
#          starting past sector 2^32, which the sixteen-byte commands reach ----
BIG="$DIR/stick-big.img"
rm -f "$BIG"
dd if=/dev/zero of="$BIG" bs=1 count=0 seek=$((4823449600 * 512)) 2>/dev/null || fail "the big stick's image"
"$MKGPT" "$BIG" 4294969344:65536:fat=FARAWAY > /dev/null || fail "mkgpt big"
# mtools cannot seek past two terabytes, so the volume is made on its own
# and copied in, and read back the same way.
rm -f "$BIG.vol"
dd if=/dev/zero of="$BIG.vol" bs=512 count=65536 2>/dev/null || fail "the far volume"
mformat -i "$BIG.vol" -F -T 65536 -v FARAWAY :: || fail "mformat big"
dd if="$BIG.vol" of="$BIG" bs=512 seek=4294969344 conv=notrunc 2>/dev/null || fail "the far volume into the image"
LOG="$DIR/big.log"
if run "$BIG" --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe big" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "a stick past two terabytes is listed at its size and its far partition written: OK"
else
    fail "the big stick boot"
fi
dd if="$BIG" of="$BIG.vol" bs=512 skip=4294969344 count=65536 2>/dev/null || fail "the far volume out of the image"
mtype -i "$BIG.vol" ::/far.txt 2>/dev/null | grep -q "written past two terabytes" \
    || fail "the file past two terabytes is not on the image"
note "and the host reads the file back past two terabytes: OK"
rm -f "$BIG" "$BIG.vol"

# ---- 13: two logical units on one device, each a disk ----
cp "$MBR" "$DIR/stick-lun0.img" || fail "the first unit's image"
cp "$MBR" "$DIR/stick-lun1.img" || fail "the second unit's image"
LOG="$DIR/luns.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-bot,bus=xhci.0,id=bot \
    --drive "if=none,id=lun0,format=raw,file=$DIR/stick-lun0.img" \
    --drive "if=none,id=lun1,format=raw,file=$DIR/stick-lun1.img" \
    --qemu-arg=-device --qemu-arg=scsi-hd,bus=bot.0,scsi-id=0,lun=0,drive=lun0 \
    --qemu-arg=-device --qemu-arg=scsi-hd,bus=bot.0,scsi-id=0,lun=1,drive=lun1 \
    --timeout 180 --steptmo 60 --ok "USBDISKPROBE OK" --never "USBDISKPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /usbdiskprobe luns" "expect=USBDISKPROBE OK" > /dev/null 2>&1; then
    note "two logical units on one device are two disks, both mounted: OK"
else
    fail "the two-unit device boot"
fi

note "PASS"
