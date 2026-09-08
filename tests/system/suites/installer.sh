#!/usr/bin/env sh

#
# installer.sh: the stick a person installs Rach2 from, and the program on it.
#
# The stick is the recovery ISO followed by an exFAT volume carrying the sealed
# disk image and a manifest naming that image's length and SHA-256
# (tools/image/mkinstallstick.sh). What is proved here is that the stick boots
# through the firmware with no kernel given to the emulator, that the machine it
# boots comes up as recovery with the stick as its own disk, and that the volume
# holds the image at the length the manifest declares.
#
# The image is copied before it is booted, since mounting the volume writes its
# dirty flag and the built image is the record.
#
# Usage:
#   installer.sh <stick-image> <sealed-disk> <harness> <work-dir> <password>
#

set -e

STICK="$1"
DISK="$2"
HARNESS="$3"
DIR="$4"
PASSWORD="$5"

if [ -z "$PASSWORD" ]; then
    echo "usage: installer.sh <stick-image> <sealed-disk> <harness> <work-dir> <password>" >&2
    exit 2
fi

mkdir -p "$DIR"
fail=0
note() { echo "installer-test: $1"; }
bad() {
    echo "installer-test: FAIL: $1" >&2
    fail=1
}

# ---- 1: the stick boots, and the volume it carries holds the image the
#         manifest declares. No kernel is given to the emulator, so what runs
#         is what GRUB reads off the stick. ----
cp "$STICK" "$DIR/install-stick.img" || bad "the stick image could not be copied"
LOG="$DIR/installstick.log"
BYTES=$(wc -c < "$STICK" | tr -d ' ')
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/install-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --timeout 300 --steptmo 120 --ok "STICKDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=cat /mnt/rach2-install.manifest" \
    "expect=sha256 " \
    "send=set -- \`ls -l /mnt/rach2-sealed.disk\`; echo SIZE\"\":\$5" \
    "expect=SIZE:" \
    "send=umount /mnt && echo STICK\"\"DONE" \
    "expect=STICKDONE" > /dev/null 2>&1; then
    note "the install stick boots to recovery and carries its image: OK"
else
    bad "the boot from the install stick"
fi

# Three lengths must agree: the image the host built, the length the manifest
# on the stick declares, and the length the file on the volume has. A stick
# whose manifest and image disagree is one the installer must refuse, and a
# stick whose image is not the one built here proves nothing about either.
HOST_BYTES=$(wc -c < "$DISK" | tr -d ' ')
MANIFEST_BYTES=$(grep -a '^bytes ' "$LOG" | head -1 | tr -d '\r' | cut -d' ' -f2)
GUEST_BYTES=$(grep -a '^SIZE:' "$LOG" | head -1 | tr -d '\r' | cut -d: -f2)
if [ "$HOST_BYTES" = "$MANIFEST_BYTES" ] && [ "$HOST_BYTES" = "$GUEST_BYTES" ]; then
    note "and the image is $GUEST_BYTES bytes on the volume, in the manifest and on the host: OK"
else
    bad "the host built $HOST_BYTES bytes, the manifest declares $MANIFEST_BYTES, the volume holds $GUEST_BYTES"
fi

# And the manifest's hash is the image's, as the host computes it.
HOST_SUM=$(shasum -a 256 "$DISK" | cut -d' ' -f1)
MANIFEST_SUM=$(grep -a '^sha256 ' "$LOG" | head -1 | tr -d '\r' | cut -d' ' -f2)
if [ "$HOST_SUM" = "$MANIFEST_SUM" ]; then
    note "and the manifest's hash is the image's: OK"
else
    bad "the manifest's hash is $MANIFEST_SUM and the image's is $HOST_SUM"
fi

# The stick the machine booted from is its own disk, whole, with two partitions.
if grep -aq "usbdisk: disk0: mbr: $((BYTES / 1048576)) MiB, 2 partitions" "$LOG"; then
    note "and the stick it booted from is its disk0 with two partitions: OK"
else
    bad "the stick was not published whole with two partitions"
fi

# ---- 2: the two halves of a raw write, on the machine that does it. A blank
#         disk is attached beside the stick; thirty-two mebibytes of the image
#         are written to it with `reflash -p`, which must say how far it has
#         got as it goes, and read back with `reflash --verify`, whose digest
#         must be the digest of the same bytes taken by another program. Two
#         mebibytes are then written at a later offset and verified there, so a
#         digest that ignored the offset would be caught. ----
rm -f "$DIR/target.img"
dd if=/dev/zero of="$DIR/target.img" bs=1048576 count=64 2>/dev/null
LOG2="$DIR/reflash.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/install-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive \
    --qemu-arg="file=$DIR/target.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "REFLASHDONE" --log "$LOG2" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /mnt && mount -t exfat /dev/disk1p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 count=32 2>/dev/null | sha256sum" \
    "expect= -" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 count=32 2>/dev/null | reflash -p /dev/disk0 0 33554432" \
    "expect=wrote 33554432 bytes" \
    "send=reflash --verify /dev/disk0 0 33554432" \
    "expect=reflash: sha256 " \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 skip=40 count=2 2>/dev/null | sha256sum" \
    "expect= -" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 skip=40 count=2 2>/dev/null | reflash /dev/disk0 33554432 2097152" \
    "expect=wrote 2097152 bytes" \
    "send=reflash --verify /dev/disk0 33554432 2097152" \
    "expect=reflash: sha256 " \
    "send=umount /mnt && echo REFLASH\"\"DONE" \
    "expect=REFLASHDONE" > /dev/null 2>&1; then
    note "the image is written to a disk and read back on the machine that wrote it: OK"
else
    bad "the raw write and its readback"
fi

# The digest reflash read back must be the one another program takes of the
# same bytes. Both come out of the same boot, so a wrong digest on both sides
# would have to be two faults that agree.
SUM_SHA=$(grep -a '^[0-9a-f]\{64\}  -' "$LOG2" | head -1 | cut -d' ' -f1)
SUM_REFLASH=$(grep -a 'reflash: sha256 ' "$LOG2" | head -1 | cut -d' ' -f3)
if [ -n "$SUM_SHA" ] && [ "$SUM_SHA" = "$SUM_REFLASH" ]; then
    note "and the digest of what reached the disk is the digest of what was sent: OK"
else
    bad "sha256sum read $SUM_SHA and reflash read $SUM_REFLASH"
fi

# The second write is at another offset and holds bytes from another part of
# the image, so a readback that ignored the offset would give the digest of the
# first thirty-two mebibytes' beginning instead of this one's.
SUM_SHA2=$(grep -a '^[0-9a-f]\{64\}  -' "$LOG2" | sed -n '2p' | cut -d' ' -f1)
SUM_SECOND=$(grep -a 'reflash: sha256 ' "$LOG2" | sed -n '2p' | cut -d' ' -f3)
if [ -n "$SUM_SHA2" ] && [ "$SUM_SHA2" = "$SUM_SECOND" ] && [ "$SUM_SECOND" != "$SUM_REFLASH" ]; then
    note "and a readback at another offset is the digest of what was written there: OK"
else
    bad "the readback at 33554432 gave $SUM_SECOND and the bytes written there hash to $SUM_SHA2"
fi

# The progress lines: one every sixteen mebibytes and one at the end, and none
# at all for the write that was not asked for them.
STEPS=$(grep -ac 'reflash: [0-9]* of 33554432 bytes' "$LOG2" || true)
if [ "$STEPS" -ge 2 ]; then
    note "and the write said how far it had got $STEPS times: OK"
else
    bad "the progress lines were not printed ($STEPS of them)"
fi
if grep -aq 'of 2097152 bytes, ' "$LOG2"; then
    bad "a write that was not asked to report its progress reported it"
else
    note "and a write not asked for them printed none: OK"
fi

if [ "$fail" -ne 0 ]; then
    echo "installer-test: FAIL" >&2
    exit 1
fi
echo "installer-test: PASS"
