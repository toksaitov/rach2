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
#       <test-stick> <test-disk> <ssh-port> <ptydrive> <demo-key>
#
# The first stick is the product's, carrying the sealed disk; the second
# carries a 65 MiB disk instead, so the rows that install do not each write a
# gibibyte and a half.
#

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

STICK="$1"
DISK="$2"
HARNESS="$3"
DIR="$4"
PASSWORD="$5"
TEST_STICK="$6"
TEST_DISK="$7"
PORT="$8"
PTYDRIVE="$9"
DEMO_KEY="${10}"
PART="${11:-all}"
INSTALLER_EXT2="${12:-}"

if [ -z "$DEMO_KEY" ]; then
    echo "usage: installer.sh <stick-image> <sealed-disk> <harness> <work-dir>" \
         "<password> <test-stick> <test-disk> <ssh-port> <ptydrive> <demo-key>" >&2
    exit 2
fi
KNOWN="$DIR/known_hosts"

mkdir -p "$DIR"

# A machine left running by an earlier run of this suite holds the images this
# one writes, and QEMU refuses to open a file another process has locked. Any
# such machine is this suite's own, since the paths are its own.
pkill -f "$DIR/" 2>/dev/null || true
sleep 2

# Change one digit of the hash the manifest on a stick image declares. The
# manifest is a few lines of text in the volume, so its bytes are found in the
# image and one of them is written: what a stick damaged in the writing or in a
# pocket looks like from the machine's side.
spoil_manifest() { # spoil_manifest IMAGE
    # The manifest is found by its first line and the hash's place computed
    # from there, since the word `sha256` appears in the programs the volume
    # carries as well: `image NAME\n`, then `bytes N\n`, then `sha256 `.
    name=$(basename "$TEST_DISK")
    bytes=$(wc -c < "$TEST_DISK" | tr -d ' ')
    at=$(grep -abo "image $name" "$1" | head -1 | cut -d: -f1)
    [ -n "$at" ] || return 1
    at=$((at + 6 + ${#name} + 1 + 6 + ${#bytes} + 1 + 7))
    was=$(dd if="$1" bs=1 skip="$at" count=1 2>/dev/null)
    if [ "$was" = "a" ]; then
        now=b
    else
        now=a
    fi
    printf '%s' "$now" | dd of="$1" bs=1 seek="$at" count=1 conv=notrunc 2>/dev/null
}

fail=0
note() { echo "installer-test: $1"; }
# The time each row took, printed as it ends, so a set shows where the
# minutes go: the guest's hashing of the image in the rows that install or
# restore, and not the setup.
MARK=$(date +%s)
mark() { now=$(date +%s); note "$1: $((now - MARK))s"; MARK=$now; }
bad() {
    echo "installer-test: FAIL: $1" >&2
    fail=1
}

if [ "$PART" = "all" ] || [ "$PART" = "install" ]; then
MARK=$(date +%s)

# ---- 1: the stick boots, and the volume it carries holds the image the
#         manifest declares. No kernel is given to the emulator, so what runs
#         is what GRUB reads off the stick. ----
cp "$STICK" "$DIR/install-stick.img" || bad "the stick image could not be copied"
mark "setup"
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

mark "row 1"
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
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 count=32 2>/dev/null | sha256sum; echo SUM\"\"ONE" \
    "expect=SUMONE" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 count=32 2>/dev/null | reflash -p /dev/disk0 0 33554432" \
    "expect=wrote 33554432 bytes" \
    "send=reflash --verify /dev/disk0 0 33554432; echo VERIFY\"\"ONE" \
    "expect=VERIFYONE" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 skip=40 count=2 2>/dev/null | sha256sum; echo SUM\"\"TWO" \
    "expect=SUMTWO" \
    "send=dd if=/mnt/rach2-sealed.disk bs=1048576 skip=40 count=2 2>/dev/null | reflash /dev/disk0 33554432 2097152" \
    "expect=wrote 2097152 bytes" \
    "send=reflash --verify /dev/disk0 33554432 2097152; echo VERIFY\"\"TWO" \
    "expect=VERIFYTWO" \
    "send=umount /mnt && echo REFLASH\"\"DONE" \
    "expect=REFLASHDONE" > "$LOG2.harness" 2>&1; then
    note "the image is written to a disk and read back on the machine that wrote it: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG2.harness" >&2
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

mark "row 2"
# ---- 3: the installer, on a machine with two blank disks. It must name both
#         as candidates and the stick as the source, refuse an answer that is
#         not one of them, refuse a confirmation that is not the disk's own
#         name, and then write the one it was told to and leave the other as it
#         was. ----
NEWPASS=an-installed-machine
USERNAME=alice
USERPASS=alices-own-password
# The disks are filled with a pattern and not with zeros: a stray write of
# zeros into a zeroed disk leaves no trace, and every check below that says a
# disk was untouched is a comparison of the whole of it.
rm -f "$DIR/one.img" "$DIR/two.img" "$DIR/pattern"
awk 'BEGIN { for (i = 0; i < 65536; i++) printf "%016d", i }' > "$DIR/pattern"
: > "$DIR/one.img"
i=0
while [ "$i" -lt 256 ]; do
    cat "$DIR/pattern" >> "$DIR/one.img"
    i=$((i + 1))
done
cp "$DIR/one.img" "$DIR/two.img"
BLANK=$(shasum -a 256 "$DIR/one.img" | cut -d' ' -f1)
cp "$TEST_STICK" "$DIR/test-stick.img"
LOG3="$DIR/install.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --qemu-arg=-drive --qemu-arg="file=$DIR/two.img,format=raw,if=ide,index=1,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "INSTALLDONE" --log "$LOG3" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=" \
    "expect=Nothing was typed" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk9" \
    "expect=is not in the list" \
    "expect=disk shall Rach2 be written to" "send=2" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=different" \
    "expect=Those two were different" \
    "expect=the password for root" "send=" \
    "expect=again" "send=" \
    "expect=An empty password will not do" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=ROOT" \
    "expect=names are lowercase" \
    "expect=The name of the first account" "send=root" \
    "expect=root is made by this install already" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=/no/such/key" \
    "expect=is not a file" \
    "expect=uthorized keys" "send=" \
    "expect=row the root" "send=no" \
    "expect=Check this before you go on" \
    "expect=Root partition: left at the size" \
    "expect=to go on, or stop to stop" "send=yes" \
    "expect=That is not /dev/disk1" \
    "expect=to go on, or stop to stop" "send=stop" \
    "expect=nothing was written" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk1" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=" \
    "expect=row the root" "send=" \
    "expect=Check this before you go on" \
    "expect=to go on, or stop to stop" "send=/dev/disk1" \
    "expect=the image on the stick is the one the manifest describes" \
    "expect=what reached the disk is the image" \
    "expect=the table is the one the image carries" \
    "expect=the accounts are root and $USERNAME" \
    "expect=growing the root into the rest of the disk" \
    "expect=installed:" "send=echo INSTALL\"\"DONE" \
    "expect=INSTALLDONE" > "$LOG3.harness" 2>&1; then
    note "the installer says what is wrong and asks again, stops when told to, and writes the disk it was told: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG3.harness" >&2
    bad "the install onto the second of two disks"
fi
# The disk it was not told to write is as it was, compared whole. The one it
# did write is not compared with the image, since the accounts were written
# into its root after the image reached it; that the image reached it whole is
# what the readback inside the installer says, and row 4 boots what is there.
if [ "$(shasum -a 256 "$DIR/one.img" | cut -d' ' -f1)" = "$BLANK" ]; then
    note "and the disk it was not told to write is as it was, byte for byte: OK"
else
    bad "the other disk was written"
fi
# The nodes the reread published are the image's four partitions.
if [ "$(grep -ac ' part 1 ' "$LOG3")" -ge 2 ]; then
    note "and the new table's partitions were published: OK"
else
    bad "the new table was not published"
fi

mark "row 3"
# ---- 3b: the same machine, the other disk. The disks are the same two in the
#          same order, and the answer names the first this time, so what
#          decides which disk is written is the name typed and not the
#          position. The one written in row 3 must be left as row 3 left it.
BEFORE3B=$(shasum -a 256 "$DIR/two.img" | cut -d' ' -f1)
LOG3B="$DIR/install-other.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --qemu-arg=-drive --qemu-arg="file=$DIR/two.img,format=raw,if=ide,index=1,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "OTHERDONE" --log "$LOG3B" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=/root/.ssh/authorized_keys" \
    "expect=row the root" "send=no" \
    "expect=to go on, or stop to stop" "send=/dev/disk0" \
    "expect=the image on the stick is the one the manifest describes" \
    "expect=what reached the disk is the image" \
    "expect=installed:" "send=echo OTHER\"\"DONE" \
    "expect=OTHERDONE" > "$LOG3B.harness" 2>&1; then
    note "the other disk of the same machine takes the same install: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG3B.harness" >&2
    bad "the install onto the first of two disks"
fi
# The disk installed in row 3 is untouched by the install onto the other one,
# compared whole: an installer that wrote accounts or a table on both would
# pass a comparison of their first sectors alone.
if [ "$(shasum -a 256 "$DIR/two.img" | cut -d' ' -f1)" = "$BEFORE3B" ]; then
    note "and the disk installed before it is untouched, byte for byte: OK"
else
    bad "the install onto the first disk changed the second"
fi
# Both disks carry the image's boot code now, each written when it was named.
# The comparison is of the 440 bytes before the table, since the disk grown in
# row 3 has a partition entry the image does not: that is the grow, and row 4
# boots what it left.
BOOT1=$(dd if="$DIR/one.img" bs=440 count=1 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
BOOT2=$(dd if="$DIR/two.img" bs=440 count=1 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
BOOTIMG=$(dd if="$TEST_DISK" bs=440 count=1 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ "$BOOT1" = "$BOOTIMG" ] && [ "$BOOT2" = "$BOOTIMG" ]; then
    note "and both disks now carry the image, each written when it was named: OK"
else
    bad "the first disk begins $BOOT1 and the second $BOOT2, and the image $BOOTIMG"
fi
# The disk that was grown has a larger root than the image; the one that was
# not has the image's own. The entry's sector count is at offset 12 of the
# fourth entry, which begins at 446 + 48.
GROWN=$(dd if="$DIR/two.img" bs=1 skip=506 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
KEPT=$(dd if="$DIR/one.img" bs=1 skip=506 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
if [ -n "$GROWN" ] && [ "$GROWN" -gt "$KEPT" ]; then
    note "and the root of the disk that was told to grow is larger than the one that was not: OK"
else
    bad "the grown root is $GROWN sectors and the one left alone is $KEPT"
fi

mark "row 3b"
# ---- 4: the machine written in row 3 boots on its own, with no stick and no
#         kernel given to the emulator. ----
LOG4="$DIR/installed.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/two.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "INSTALLEDUP" --log "$LOG4" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=incorrect" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=id -u; grep -c . /etc/passwd" \
    "expect=0" \
    "send=grep -q '^root-password-login yes' /etc/rach2/sshd.conf && echo POLICY\"\"SET" \
    "expect=POLICYSET" \
    "send=test -d /home/$USERNAME && test ! -d /root/.ssh && echo HOME\"\"MADE" \
    "expect=HOMEMADE" \
    "send=df -k / | tail -1" \
    "expect=/dev/" \
    "send=mkdir -p /r && mount -t vfat /dev/disk0p3 /r && cmp /r/rach2-config/etc/shadow /etc/shadow && grep -c ^$USERNAME: /r/rach2-config/etc/passwd; umount /r" \
    "expect=1" \
    "send=echo INSTALLED\"\"UP" \
    "expect=INSTALLEDUP" > /dev/null 2>&1; then
    note "and the disk it wrote boots, refuses the image's own password and takes the new one: OK"
    note "and the recovery partition carries the archive of its accounts: OK"
else
    bad "the boot of the installed disk"
fi

mark "row 4"
# ---- 5: a disk with something mounted from it is not offered. The stick's own
#         volume is mounted first, which makes the stick a disk with a mount as
#         well as the source. ----
LOG5="$DIR/mounted.log"
cp "$TEST_STICK" "$DIR/mounted-stick.img"
rm -f "$DIR/three.img"
cp "$DIR/two.img" "$DIR/three.img"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/mounted-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/three.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "MOUNTEDDONE" --log "$LOG5" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /mnt/held && mount -t vfat /dev/disk0p1 /mnt/held && echo HELD" \
    "expect=HELD" "send=rach2-install" \
    "expect=has something mounted from it" \
    "expect=no disk of this machine can take the image" \
    "send=umount /mnt/held; echo MOUNTED\"\"DONE" \
    "expect=MOUNTEDDONE" > /dev/null 2>&1; then
    note "a disk with something mounted from it is named and not offered: OK"
else
    bad "the refusal of a disk with a mount"
fi

mark "row 5"
# ---- 6: the accounts an install writes, over SSH. The install in row 3b was
#         given the recovery system's own authorized keys as the account's, so
#         the host's demo private key opens that account; root's password is
#         the one typed at the install, and root's keys are gone, since the
#         image's demonstration keys are not this machine's. The SSH server is
#         started by `init` and not by hand, which is what puts the policy the
#         installer wrote under test: without it dropbear runs with `-g` and
#         refuses root a password. ----
rm -f "$KNOWN"
LOG6="$DIR/ssh.log"
"$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --net "user,id=net0,hostfwd=tcp::$PORT-:22" --device "rtl8139,netdev=net0" \
    --timeout 300 --steptmo 120 --ok "SSHREADY" --log "$LOG6" \
    "expect=rach2 login:" wait=25 \
    "send=" "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=echo SSH\"\"READY" "expect=SSHREADY" until=90 \
    > "$LOG6.harness" 2>&1 &
QPID=$!

# The three attempts, while that machine is up. Each says what it proves.
sleep "$(scaled 60)"
OUT=$(RACH2_PASSWORD="$NEWPASS" "$PTYDRIVE" --rows 40 --cols 100 --timeout 60 -- \
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o "ConnectTimeout=$(scaled 20)" -o PubkeyAuthentication=no \
    -o PreferredAuthentications=password -o NumberOfPasswordPrompts=3 -tt \
    -p "$PORT" root@localhost 'id -u; echo ROOT""PASSWORD' 2>&1 || true)
case "$OUT" in
    *ROOTPASSWORD*) note "root logs in over SSH with the password the install was given: OK" ;;
    *) bad "root's password login over SSH: $OUT" ;;
esac
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
    -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
    -i "$DEMO_KEY" -p "$PORT" "$USERNAME@localhost" 'id -un; echo KEY""LOGIN' 2>&1 || true)
case "$OUT" in
    *KEYLOGIN*) note "and the account logs in with the keys the install was given: OK" ;;
    *) bad "the account's key login: $OUT" ;;
esac
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
    -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
    -i "$DEMO_KEY" -p "$PORT" root@localhost 'echo DEMO""KEY' 2>&1 || true)
# The server must have refused it, not merely failed to answer: a machine that
# had gone would pass a check that only looked for the token's absence.
case "$OUT" in
    *DEMOKEY*) bad "the image's demonstration key still opens root on the installed machine" ;;
    *"Permission denied"*) note "and the image's own key is refused for root: OK" ;;
    *) bad "the demonstration key's attempt neither ran nor was refused: $OUT" ;;
esac
wait "$QPID" || bad "the installed machine did not stay up for the SSH attempts"

mark "row 6"
# ---- 7: a stick whose image is not the one its manifest describes. The
#         manifest's hash is changed on the host, one digit of it, which is
#         what a stick damaged in the writing or in a pocket looks like from
#         the machine's side. Nothing may be written. ----
cp "$TEST_STICK" "$DIR/bad-stick.img"
spoil_manifest "$DIR/bad-stick.img" || bad "the manifest on the copy could not be changed"
rm -f "$DIR/four.img"
cp "$DIR/one.img" "$DIR/four.img"
FOUR=$(shasum -a 256 "$DIR/four.img" | cut -d' ' -f1)
LOG7="$DIR/badhash.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/bad-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/four.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "BADHASHDONE" --log "$LOG7" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=" \
    "expect=row the root" "send=no" \
    "expect=to go on, or stop to stop" "send=/dev/disk0" \
    "expect=and the manifest says" \
    "expect=# " "send=echo BADHASH\"\"DONE" \
    "expect=BADHASHDONE" > "$LOG7.harness" 2>&1; then
    note "a stick whose image is not the one its manifest describes is refused: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG7.harness" >&2
    bad "the refusal of a damaged stick"
fi
if [ "$(shasum -a 256 "$DIR/four.img" | cut -d' ' -f1)" = "$FOUR" ]; then
    note "and the disk it was going to write is as it was, byte for byte: OK"
else
    bad "the disk was written from a stick whose image did not match its manifest"
fi

mark "row 7"
# ---- 8: the stick alone, with no disk to write. The machine must say so and
#         stop, rather than offering the stick it is running from. ----
LOG8="$DIR/nodisk.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --timeout 300 --steptmo 120 --ok "NODISKDONE" --log "$LOG8" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=is the stick this is running from" \
    "expect=no disk of this machine can take the image" \
    "expect=# " "send=echo NODISK\"\"DONE" \
    "expect=NODISKDONE" > /dev/null 2>&1; then
    note "a machine with nothing but the stick is told so and nothing is written: OK"
else
    bad "the machine with no disk to write"
fi

mark "row 8"
fi

if [ "$PART" = "all" ] || [ "$PART" = "repair" ]; then
MARK=$(date +%s)

# The disk this half repairs is a disk with nothing on it: what a person has
# after a machine's disk was destroyed, or a disk they have just put in. The
# install that would otherwise have to run first is the other half's subject.
NEWPASS=an-installed-machine
USERNAME=alice
USERPASS=alices-own-password
cp "$TEST_STICK" "$DIR/test-stick.img"
rm -f "$DIR/one.img" "$DIR/pattern"
awk 'BEGIN { for (i = 0; i < 65536; i++) printf "%016d", i }' > "$DIR/pattern"
: > "$DIR/one.img"
i=0
while [ "$i" -lt 256 ]; do
    cat "$DIR/pattern" >> "$DIR/one.img"
    i=$((i + 1))
done
BOOTIMG=$(dd if="$TEST_DISK" bs=440 count=1 2>/dev/null | shasum -a 256 | cut -d' ' -f1)

mark "setup"
# ---- 9: a machine whose disk has been destroyed, brought back through the
#         recovery menu. The first mebibyte of the disk installed in row 3b is
#         zeroed on the host, which takes its boot code, its table and the
#         start of its first partition; the stick is booted on it, and
#         `rach2-recover` puts the stick's own image back and reads it back.
#         The disk must then boot again. ----
# First a write killed while it is running, at the layer that does the writing:
# twenty-four mebibytes of the image are sent to the disk and the machine is
# killed once it says it has passed sixteen. What is left is neither the disk
# that was there nor the image, which is what a person who pulled the plug
# during an install comes back to, and the restore below is what puts it right.
#
# It is `reflash` and not the menu because reaching the menu's own write costs
# ten minutes of hashing in the emulator, and what is being shown is the state
# a killed write leaves, not which program was running it.
cp "$DIR/one.img" "$DIR/cut.img"
dd if="$DIR/cut.img" bs=1048576 count=96 of="$DIR/pattern-96" 2>/dev/null
rm -f "$DIR/cutoff.log"
"$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/cut.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 1200 --steptmo 600 --ok "NEVERSEEN" --log "$DIR/cutoff.log" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=dd if=/m/$(basename "$TEST_DISK") bs=1048576 count=96 2>/dev/null | reflash -p /dev/disk0 0 100663296" \
    "expect=NEVERSEEN" > /dev/null 2>&1 &
CUTPID=$!
waited=0
saw_progress=no
while [ "$waited" -lt "$(scaled 300)" ]; do
    if grep -aq 'reflash: 16777216 of ' "$DIR/cutoff.log" 2>/dev/null; then
        saw_progress=yes
        break
    fi
    sleep 2
    waited=$((waited + 2))
done
# The harness's own child holds the disk and the stick; both are named, since a
# machine left running would hold them for every later row. Every one of these
# may find nothing left to kill, which is not a failure of the row.
kill "$CUTPID" 2>/dev/null || true
pkill -f "file=$DIR/cut.img" 2>/dev/null || true
wait "$CUTPID" 2>/dev/null || true
# The machine writes through the emulator's own cache, so what is on the host
# is what QEMU has flushed; the disk is read after that process has gone. It is
# a disk of this row's own, so a machine slow to die holds nothing the rows
# after it open.
gone=0
while [ "$gone" -lt "$(scaled 60)" ]; do
    pgrep -f "file=$DIR/cut.img" > /dev/null 2>&1 || break
    sleep 1
    gone=$((gone + 1))
done
sleep 2
# What is judged is the ninety-six mebibytes the write asked for: it must have
# begun, so those bytes are not what was there before, and it must not have
# finished, so they are not the image's either. A comparison of the whole disk
# would pass whatever happened, since the disk is larger than the image.
STARTED=$(dd if="$DIR/cut.img" bs=1048576 count=96 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
WAS=$(dd if="$DIR/pattern-96" bs=1048576 count=96 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
WHOLE=$(dd if="$TEST_DISK" bs=1048576 count=96 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ "$saw_progress" != yes ]; then
    bad "the write never said it had got anywhere, so it was not killed while it ran"
elif [ "$STARTED" = "$WAS" ]; then
    bad "the write that was killed had not begun"
elif [ "$STARTED" = "$WHOLE" ]; then
    bad "the write that was killed had finished"
else
    note "a write killed while it was running leaves the range it asked for part written: OK"
fi

LOG9="$DIR/recover.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "RECOVERDONE" --log "$LOG9" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-recover" \
    "expect=which?" "send=1" \
    "expect=/dev/disk0:" \
    "expect=which?" "send=3" \
    "expect=which disk shall be restored" "send=/dev/disk0" \
    "expect=the image to put on it" "send=" \
    "expect=to go on, or stop to stop" "send=/dev/disk0" \
    "expect=reading the image to check it against its hash" \
    "expect=reflash: 16777216 of " \
    "expect=restored and read back" \
    "expect=which?" "send=6" \
    "expect=leaving" "send=echo RECOVER\"\"DONE" \
    "expect=RECOVERDONE" > "$LOG9.harness" 2>&1; then
    note "a destroyed disk is put back through the recovery menu: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG9.harness" >&2
    bad "the restore through the recovery menu"
fi
BOOT9=$(dd if="$DIR/one.img" bs=440 count=1 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ "$BOOT9" = "$BOOTIMG" ]; then
    note "and the disk carries the image's boot code again: OK"
else
    bad "the restored disk begins $BOOT9 and the image $BOOTIMG"
fi
LOG10="$DIR/recovered.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "RECOVEREDUP" --log "$LOG10" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=echo RECOVERED\"\"UP" \
    "expect=RECOVEREDUP" > /dev/null 2>&1; then
    note "and the machine boots what was put back, with the image's own accounts: OK"
else
    bad "the boot of the restored disk"
fi

mark "row 9"
# ---- 11: a machine changed after it was installed, and then updated. On the
#          running machine root's password is changed, root's keys are removed
#          as an install removes them, an account's home is made, and a file
#          outside everything the update keeps is marked. The update must then
#          leave the password, the absent keys and the home as they are, and
#          must replace the marked file with the image's, which is what says
#          the root was written at all.
LOG11="$DIR/changed.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "CHANGEDDONE" --log "$LOG11" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=echo root:$NEWPASS | chpasswd && echo PASSWORD\"\"SET" \
    "expect=PASSWORDSET" \
    "send=rm -rf /root/.ssh && mkdir -p /home/$USERNAME && chown 1000:1000 /home/$USERNAME" \
    "send=echo this-machines-own > /home/$USERNAME/mine && chown 1000:1000 /home/$USERNAME/mine" \
    "send=echo a-marked-motd > /etc/motd && echo CHANGED\"\"DONE" \
    "expect=CHANGEDDONE" \
    "send=mkdir -p /r && mount -t vfat /dev/disk0p3 /r && mkdir -p /r/rach2-config/etc && echo tampered:x:99: >> /r/rach2-config/etc/group && umount /r && echo ARCHIVE\"\"TAMPERED" \
    "expect=ARCHIVETAMPERED" > /dev/null 2>&1; then
    note "the installed machine is given a password, an account's home and a marked file, and its archive a difference: OK"
else
    bad "changing the installed machine"
fi

LOG12="$DIR/update.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "UPDATEDONE" --log "$LOG12" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0" \
    "expect=copying this machine's accounts" \
    "expect=differs from the root's etc/group; the root's is kept" \
    "expect=what reached the root is the image" \
    "expect=writing the accounts, the policy and the keys back" \
    "expect=the configuration is archived on" \
    "expect=armed for one trial boot" \
    "expect=updated:" "send=umount /m; echo UPDATE\"\"DONE" \
    "expect=UPDATEDONE" > "$LOG12.harness" 2>&1; then
    note "a machine that is already installed takes a new root, keeps its accounts, and names the archive's difference: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG12.harness" >&2
    bad "the update of an installed machine"
fi

LOG13="$DIR/updated.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "UPDATEDUP" --log "$LOG13" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=incorrect" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=cat /etc/motd" "expect=Welcome to Rach2" \
    "send=test ! -d /root/.ssh && echo KEYS\"\"GONE" "expect=KEYSGONE" \
    "send=cat /home/$USERNAME/mine; stat -c %u /home/$USERNAME/mine" \
    "expect=this-machines-own" "expect=1000" \
    "send=echo UPDATED\"\"UP" \
    "expect=UPDATEDUP" > /dev/null 2>&1; then
    note "and it takes the password it had, keeps the home and its owner, and has the image's \
own motd where the marked one was: OK"
else
    bad "the boot of the updated machine"
fi

mark "row 11"
# ---- 14: the root damaged, and the machine updated from the archive: the
#          superblock zeroed so the root does not mount, then the update
#          takes the accounts from the archive the last update refreshed and
#          writes the image into the last ext2 entry of the table. The
#          home directories were on the root that does not read, so they
#          are not kept. ----
LOG14="$DIR/archive-update.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "ARCHIVEDONE" --log "$LOG14" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=dd if=/dev/zero of=/dev/disk0p4 bs=1024 seek=1 count=1 && echo DAMA\"\"GED" \
    "expect=DAMAGED" \
    "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0" \
    "expect=the root does not read; taking the configuration saved on the recovery partition" \
    "expect=what reached the root is the image" \
    "expect=writing the accounts, the policy and the keys back" \
    "expect=the configuration is archived on" \
    "expect=updated:" "send=umount /m; echo ARCHIVE\"\"DONE" \
    "expect=ARCHIVEDONE" > "$LOG14.harness" 2>&1; then
    note "a machine whose root no longer reads is updated from the archive on its recovery partition: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG14.harness" >&2
    bad "the update of a machine whose root does not read"
fi

LOG15="$DIR/archive-updated.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "ARCHIVEUP" --log "$LOG15" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=grep -c ^tampered: /etc/group | sed s/^/TAMPERED=/; test ! -e /home/$USERNAME/mine && echo HOME\"\"GONE" \
    "expect=TAMPERED=0" "expect=HOMEGONE" \
    "send=echo ARCHIVE\"\"UP" \
    "expect=ARCHIVEUP" > /dev/null 2>&1; then
    note "and it takes the password the archive held, the archive refreshed by the last update and not the tampered one, and none of the home the root lost: OK"
else
    bad "the boot of the machine updated from the archive"
fi

mark "row 14"
# ---- 16: an update stopped between the root's write and the accounts'
#          return. The archive on the recovery partition is what such an
#          update wrote before the root: the machine's own accounts, marked
#          as an update in progress. Here the mark is planted on the host
#          with an account added to the archived passwd, so the retry's
#          choice shows: it must say the root holds the image's accounts and
#          take the archive, and the added account must have an empty home
#          on the new root, since the root's homes were the image's. The
#          root is written with the image on the host first, which is what
#          the stopped update left: the image's accounts, homes and keys.
REC_START=$(od -An -tu4 -j $((446 + 2 * 16 + 8)) -N 4 "$DIR/one.img" | tr -d ' ')
[ -n "$REC_START" ] && [ "$REC_START" -gt 0 ] || bad "the recovery partition's start could not be read"
mcopy -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config/etc/passwd "$DIR/archived-passwd" ||
    bad "the archived passwd could not be read off the recovery partition"
echo "extra:x:1001:1001::/home/extra:/bin/sh" >> "$DIR/archived-passwd"
echo "$(basename "$INSTALLER_EXT2") stopped" > "$DIR/update-in-progress"
ROOT4_START=$(od -An -tu4 -j $((446 + 3 * 16 + 8)) -N 4 "$DIR/one.img" | tr -d ' ')
[ -n "$ROOT4_START" ] && [ "$ROOT4_START" -gt 0 ] || bad "the root partition's start could not be read"
dd if="$INSTALLER_EXT2" of="$DIR/one.img" bs=512 seek="$ROOT4_START" conv=notrunc 2>/dev/null ||
    bad "the image could not be written onto the root, as the stopped update left it"
mcopy -o -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/archived-passwd" ::rach2-config/etc/passwd ||
    bad "the archived passwd could not be written back to the recovery partition"
mcopy -o -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/update-in-progress" ::rach2-config/update-in-progress ||
    bad "the mark could not be planted on the recovery partition"
LOG16="$DIR/stopped-update.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "STOPPEDDONE" --log "$LOG16" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0" \
    "expect=an update was stopped part way, and the root holds the image's accounts; taking the configuration saved on the recovery partition" \
    "expect=before the root is written" \
    "expect=what reached the root is the image" \
    "expect=the configuration is archived on" \
    "expect=updated:" "send=umount /m; echo STOPPED\"\"DONE" \
    "expect=STOPPEDDONE" > "$LOG16.harness" 2>&1; then
    note "an update stopped between the root's write and the accounts' return is retried from the marked archive: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG16.harness" >&2
    bad "the retry of a stopped update"
fi

LOG17="$DIR/stopped-updated.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "STOPPEDUP" --log "$LOG17" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=grep -c ^extra: /etc/passwd | sed s/^/EXTRA=/; stat -c %u:%g:%a /home/extra | sed s/^/OWNED=/; test -z \"\$(ls -A /home/extra)\" && test -z \"\$(ls -A /home/test)\" && test ! -e /root/.ssh && echo HOME\"\"MADE" \
    "expect=EXTRA=1" "expect=OWNED=1001:1001:700" "expect=HOMEMADE" \
    "send=mkdir -p /r && mount -t vfat /dev/disk0p3 /r && test ! -e /r/rach2-config/update-in-progress && echo MARK\"\"GONE; umount /r" \
    "expect=MARKGONE" \
    "send=echo STOPPED\"\"UP" \
    "expect=STOPPEDUP" > /dev/null 2>&1; then
    note "and it has the archive's added account with an empty home, the image's home emptied of its key, none of root's, and the mark taken off: OK"
else
    bad "the boot of the machine after the retried update"
fi

mark "row 16"
fi

# ---- the rehearsal of milestone 50's sitting, at the machine's own size,
#      behind RACH2_REHEARSE_LENOVO since it is hours in the emulator: a disk
#      of the Lenovo's 488397168 sectors as a sparse base under a qcow2
#      overlay, so that `qemu-img map` afterwards lists exactly the sectors
#      the rows wrote; the save set computed by `growset` from the product
#      image's ext2 head and the root's offset, with the image's own extent;
#      the ranges saved from the disk onto a save stick by the recovery
#      system, as the sitting saves them; the install through the product
#      stick with the root grown to the disk's end; every sector written
#      inside the save set; the saved ranges put back with `reflash` and read
#      back with the digests the host took of them; and every sector the
#      restore wrote inside the set too. The times are printed against the
#      sitting's ten-minute budget, which is the machine's, not the
#      emulator's. ----
if [ "$PART" = "rehearse" ] ||
    { { [ "$PART" = "all" ] || [ "$PART" = "install" ]; } &&
      { [ -n "${RACH2_REHEARSE_LENOVO:-}" ] || [ -n "${RACH2_REHEARSE_SMALL:-}" ]; }; }; then
MARK=$(date +%s)
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
GROWSET="$REPO/build/tools/inspect/growset"
# The small form, the `rehearse` part's default and RACH2_REHEARSE_SMALL
# for the other parts: the same steps on a 256 MiB disk with the test stick
# and its 138 MiB image, minutes where the machine's size is longer, which
# is what the row is proved on before RACH2_REHEARSE_LENOVO runs it at size.
if [ -z "${RACH2_REHEARSE_LENOVO:-}" ]; then
    LENOVO=524288
    STICK=$TEST_STICK
    DISK=$TEST_DISK
else
    LENOVO=488397168
fi
# The answers the installer is given, the install half's when it ran and
# these when the rehearsal runs alone.
NEWPASS=${NEWPASS:-an-installed-machine}
USERNAME=${USERNAME:-alice}
USERPASS=${USERPASS:-alices-own-password}
R="$DIR/rehearse"
rm -rf "$R"; mkdir -p "$R"
IMAGE_BYTES=$(wc -c < "$DISK" | tr -d ' ')
IMAGE_SECTORS=$((IMAGE_BYTES / 512))
# The root's entry in the image's table, the fourth: its first sector from
# the MBR's bytes.
ROOT_START=$(od -An -tu4 -j $((446 + 3 * 16 + 8)) -N 4 "$DISK" | tr -d ' ')
[ -n "$ROOT_START" ] && [ "$ROOT_START" -gt 0 ] || bad "the image's fourth entry has no start"
dd if="$DISK" of="$R/head.vol" bs=512 skip="$ROOT_START" count=$((128 * 2048)) 2>/dev/null \
    || bad "the root's head could not be read from the image"
"$GROWSET" --lba "$ROOT_START" "$R/head.vol" $((LENOVO - ROOT_START)) > "$R/save.set" \
    || bad "growset did not compute the save set"
# The ranges to save, `START COUNT` in sectors: the image's extent, then the
# grow's ranges beyond it; a range inside the extent is saved with it.
{
    echo "0 $IMAGE_SECTORS"
    awk -v n="$IMAGE_SECTORS" '$1 == "range" && $2 + $3 > n { s = $2 < n ? n : $2; print s, $2 + $3 - s }' "$R/save.set"
} > "$R/ranges"
NRANGES=$(grep -c . "$R/ranges")
note "rehearsal: the save set is the image's $IMAGE_SECTORS sectors and $((NRANGES - 1)) ranges of the grow beyond it"
mark "setup"
# The base and the overlay; the backing name is relative to the overlay's
# own directory, which is where both live. Each range's first sector on the
# base names the range, so a save that read another offset, or a restore
# that put zeros back, shows in the digests the host expects below.
dd if=/dev/zero of="$R/base.raw" bs=512 seek=$((LENOVO - 1)) count=1 2>/dev/null || bad "the base disk"
while read -r s c; do
    printf 'range %s %s\n' "$s" "$c" | dd of="$R/base.raw" bs=512 seek="$s" count=1 conv=notrunc 2>/dev/null ||
        bad "the range at $s could not be marked on the base disk"
done < "$R/ranges"
# Clusters of 4 KiB, so the overlay's map is exact to the eight sectors the
# grow's ranges are aligned to; the default 64 KiB would show every range
# rounded up to a cluster and call the rounding a write outside the set.
(cd "$R" && qemu-img create -q -f qcow2 -o cluster_size=4096 -b base.raw -F raw disk.qcow2) || bad "the overlay"
# The save disk: an exFAT volume with room for every range, the format the
# sitting's stick has, written by the machine; the list of ranges goes on it
# as its one file, one `START COUNT` a line, so the machine's own loop reads
# it and the harness types one line. It is on IDE beside the target and not
# on USB as the sitting's stick is, since the rehearsal is of the procedure
# and its set and not of the emulator's buses; the loops find it as the
# second fixed disk. It is not FAT32: this kernel writes FAT32 through a
# block at a time, 0.6 MB a second in the emulator, which is entry 25.
SAVE_SECTORS=$(( (IMAGE_SECTORS / 2048 + 2048 + 32) * 2048 ))
"$REPO/build/tests/host/mkexfatfixture" --make "$R/save.vol" --sectors "$SAVE_SECTORS" --offset 2048 \
    --add "ranges=$R/ranges" > /dev/null || bad "the save disk's volume"
{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat "$R/save.vol"; } > "$R/save-stick.img"
rm -f "$R/save.vol"
"$REPO/build/tools/image/mkmbr" --lba "$R/save-stick.img" 2048:$SAVE_SECTORS:07 > /dev/null || bad "the save disk's table"
cp "$STICK" "$R/product-stick.img" || bad "the product stick's copy"
# A boot of the recovery system from the product stick, with the overlay disk
# on IDE as disk0, the save disk on IDE as disk1 and the product stick disk2,
# which is the order this recovery kernel registers them in; the loops look
# the target up as the first fixed disk and the save disk up as the second
# rather than trusting the numbers. The steps come from a file, one a line.
# rehearse_boot LOG TIMEOUT OK STEPFILE: one boot of the recovery system with
# the disks above, its steps read from STEPFILE one a line and typed in single
# quotes, since the loops carry the machine's variables, which the host's
# shell must not expand. The command line goes to LOG.sh for the record.
rehearse_boot() {
    {
        printf '"%s" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512' "$HARNESS"
        printf ' --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0'
        printf ' --drive "if=none,id=stick,format=raw,file=%s/product-stick.img"' "$R"
        printf ' --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0'
        printf ' --qemu-arg=-drive --qemu-arg="file=%s/disk.qcow2,format=qcow2,if=ide,index=0,media=disk"' "$R"
        printf ' --qemu-arg=-drive --qemu-arg="file=%s/save-stick.img,format=raw,if=ide,index=1,media=disk"' "$R"
        printf ' --timeout %s --steptmo %s --ok "%s" --log "%s"' "$2" "$2" "$3" "$1"
        printf ' "expect=rach2 login:" "send=root" "expect=Password:" "send=%s" "expect=# "' "$PASSWORD"
        while IFS= read -r step; do
            printf " '%s'" "$step"
        done < "$4"
        printf '\n'
    } > "$1.sh"
    sh "$1.sh"
}
# The save: each range read off the disk into a file of its own on the save
# stick, by the machine's loop over the list. The input block is the largest
# the range's alignment allows, since dd is a system call per block and a
# range of a gibibyte in 512-byte blocks is hours on the machine as well as
# in the emulator; the ranges the grow writes are aligned to eight sectors.
# The output block is a mebibyte whatever the input's, because the file
# system syncs and flushes the disk at every write call, and a 4 KiB write
# per call would cost a flush per page of the save.
{
    # shellcheck disable=SC2016
    echo 'send=v=$(grep ^identity /proc/disk | grep " fixed " | sed -n 2p | cut -d" " -f2); mkdir -p /save && mount -t exfat /dev/disk${v}p1 /save && echo SAVE""MOUNTED'
    echo 'expect=SAVEMOUNTED'
    # shellcheck disable=SC2016
    echo 'send=d=$(grep ^identity /proc/disk | grep " fixed " | head -1 | cut -d" " -f2); i=0; while read s c; do u=1; for t in 2048 8; do if [ $((s % t)) = 0 ] && [ $((c % t)) = 0 ] && [ $u = 1 ]; then u=$t; fi; done; dd if=/dev/disk$d of=/save/range-$i ibs=$((u * 512)) obs=1M skip=$((s / u)) count=$((c / u)) 2>/dev/null || echo SAVEFAIL $i; i=$((i + 1)); done < /save/ranges; sync; echo SAVED""ALL $i'
    echo "expect=SAVEDALL $NRANGES"
    echo 'send=umount /save && echo SAVE""DONE'
    echo 'expect=SAVEDONE'
} > "$R/save.steps"
T0=$(date +%s)
if rehearse_boot "$R/save.log" 21600 SAVEDONE "$R/save.steps" > "$R/save.harness" 2>&1 &&
    ! grep -aq "^SAVEFAIL " "$R/save.log"; then
    SAVE_S=$(( $(date +%s) - T0 ))
    note "rehearsal: the save of $NRANGES ranges took $SAVE_S s"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$R/save.harness" >&2
    bad "the rehearsal's save"
fi
# The install, through the product stick, the root grown to the disk's end.
{
    echo 'send=rach2-install'
    echo 'expect=disk shall Rach2 be written to'; echo 'send=/dev/disk0'
    echo 'expect=the password for root'; echo "send=$NEWPASS"
    echo 'expect=again'; echo "send=$NEWPASS"
    echo 'expect=The name of the first account'; echo "send=$USERNAME"
    echo "expect=the password for $USERNAME"; echo "send=$USERPASS"
    echo 'expect=again'; echo "send=$USERPASS"
    echo 'expect=uthorized keys'; echo 'send='
    echo 'expect=row the root'; echo 'send='
    echo 'expect=to go on, or stop to stop'; echo 'send=/dev/disk0'
    echo 'expect=the image on the stick is the one the manifest describes'
    echo 'expect=what reached the disk is the image'
    echo 'expect=the table is the one the image carries'
    echo "expect=the accounts are root and $USERNAME"
    echo 'expect=growing the root into the rest of the disk'
    echo 'expect=installed:'; echo 'send=echo INSTALL""DONE'
    echo 'expect=INSTALLDONE'
} > "$R/install.steps"
T0=$(date +%s)
if rehearse_boot "$R/install.log" 21600 INSTALLDONE "$R/install.steps" > "$R/install.harness" 2>&1; then
    INSTALL_S=$(( $(date +%s) - T0 ))
    note "rehearsal: the install and the grow took $INSTALL_S s in the emulator, against the sitting's budget of 600 s on the machine"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$R/install.harness" >&2
    bad "the rehearsal's install"
fi
# Every sector the install wrote lies in the save set: growset judges its
# own ranges from the overlay's map, and an extent it calls outside must lie
# inside the image's extent, which the install writes whole.
qemu-img map --output=json "$R/disk.qcow2" > "$R/after-install.json" || bad "the overlay's map"
grep -q '"start"' "$R/after-install.json" || bad "the overlay's map lists no extent, and the install wrote"
growset_rc=0
"$GROWSET" --lba "$ROOT_START" --map "$R/after-install.json" "$R/head.vol" $((LENOVO - ROOT_START)) > "$R/after-install.check" 2>&1 || growset_rc=$?
[ "$growset_rc" -eq 0 ] || grep -q '^outside' "$R/after-install.check" || bad "growset failed on the install's map: $(head -1 "$R/after-install.check")"
if awk -v n="$IMAGE_SECTORS" '$1 == "outside" && $2 + $3 > n { bad = 1 } END { exit bad }' "$R/after-install.check"; then
    note "rehearsal: every sector the install and the grow wrote lies in the save set: OK"
else
    grep '^outside' "$R/after-install.check" | head -5 >&2
    bad "the install wrote outside the save set"
fi
# The restore: each range put back with reflash from the save stick by the
# machine's loop, then each read back with `reflash --verify` into a file
# of digests the host compares with its own digests of the saved bytes,
# which are zeros of each length.
{
    # shellcheck disable=SC2016
    echo 'send=v=$(grep ^identity /proc/disk | grep " fixed " | sed -n 2p | cut -d" " -f2); mkdir -p /save && mount -t exfat /dev/disk${v}p1 /save && echo SAVE""MOUNTED'
    echo 'expect=SAVEMOUNTED'
    # shellcheck disable=SC2016
    echo 'send=d=$(grep ^identity /proc/disk | grep " fixed " | head -1 | cut -d" " -f2); i=0; while read s c; do reflash /dev/disk$d $((s * 512)) $((c * 512)) < /save/range-$i || echo PUTFAIL $i; i=$((i + 1)); done < /save/ranges; echo PUT""ALL $i'
    echo "expect=PUTALL $NRANGES"
    # shellcheck disable=SC2016
    echo 'send=: > /save/verify; while read s c; do reflash --verify /dev/disk$d $((s * 512)) $((c * 512)) >> /save/verify; done < /save/ranges; sync; echo VERIFY""ALL'
    echo 'expect=VERIFYALL'
    echo 'send=umount /save && echo RESTORE""DONE'
    echo 'expect=RESTOREDONE'
} > "$R/restore.steps"
T0=$(date +%s)
if rehearse_boot "$R/restore.log" 21600 RESTOREDONE "$R/restore.steps" > "$R/restore.harness" 2>&1 &&
    ! grep -aq "^PUTFAIL " "$R/restore.log"; then
    RESTORE_S=$(( $(date +%s) - T0 ))
    note "rehearsal: the restore of $NRANGES ranges and their readback took $RESTORE_S s"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$R/restore.harness" >&2
    bad "the rehearsal's restore"
fi
# What the machine read back against what was saved: each range's mark and
# then zeros to its length.
"$REPO/build/tests/host/mkexfatfixture" --check "$R/save-stick.img" 2048 --read /verify > "$R/verify.machine" 2>/dev/null || bad "the digests off the save disk"
: > "$R/verify.host"
while read -r start count; do
    mark="range $start $count"
    { printf '%s\n' "$mark"; head -c $((count * 512 - ${#mark} - 1)) /dev/zero; } |
        shasum -a 256 | cut -d' ' -f1 >> "$R/verify.host"
done < "$R/ranges"
if [ "$(tr -d '\r' < "$R/verify.machine" | awk '{print $3}')" = "$(cat "$R/verify.host")" ]; then
    note "rehearsal: every range read back as the bytes that were saved: OK"
else
    bad "the ranges read back differ from what was saved"
fi
qemu-img map --output=json "$R/disk.qcow2" > "$R/after-restore.json" || bad "the overlay's map after the restore"
growset_rc=0
"$GROWSET" --lba "$ROOT_START" --map "$R/after-restore.json" "$R/head.vol" $((LENOVO - ROOT_START)) > "$R/after-restore.check" 2>&1 || growset_rc=$?
[ "$growset_rc" -eq 0 ] || grep -q '^outside' "$R/after-restore.check" || bad "growset failed on the restore's map: $(head -1 "$R/after-restore.check")"
if awk -v n="$IMAGE_SECTORS" '$1 == "outside" && $2 + $3 > n { bad = 1 } END { exit bad }' "$R/after-restore.check"; then
    note "rehearsal: the restore wrote nothing outside the save set either: OK"
else
    bad "the restore wrote outside the save set"
fi
rm -f "$R/base.raw" "$R/disk.qcow2" "$R/save-stick.img" "$R/product-stick.img" "$R"/range-*.bin
mark "rehearsal"
fi

if [ "$fail" -ne 0 ]; then
    echo "installer-test: FAIL" >&2
    exit 1
fi
echo "installer-test: PASS"
