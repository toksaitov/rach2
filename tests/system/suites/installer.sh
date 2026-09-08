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

if [ "$fail" -ne 0 ]; then
    echo "installer-test: FAIL" >&2
    exit 1
fi
echo "installer-test: PASS"
