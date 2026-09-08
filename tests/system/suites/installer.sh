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

if [ -z "$DEMO_KEY" ]; then
    echo "usage: installer.sh <stick-image> <sealed-disk> <harness> <work-dir>" \
         "<password> <test-stick> <test-disk> <ssh-port> <ptydrive> <demo-key>" >&2
    exit 2
fi
KNOWN="$DIR/known_hosts"

mkdir -p "$DIR"

# Change one digit of the hash the manifest on a stick image declares. The
# manifest is a few lines of text in the volume, so its bytes are found in the
# image and one of them is written: what a stick damaged in the writing or in a
# pocket looks like from the machine's side.
spoil_manifest() { # spoil_manifest IMAGE
    # The last match is the manifest's: the ones before it are inside the
    # recovery system's own programs, which name the algorithm in their text.
    at=$(grep -abo 'sha256 ' "$1" | tail -1 | cut -d: -f1)
    [ -n "$at" ] || return 1
    at=$((at + 7))
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

# ---- 3: the installer, on a machine with two blank disks. It must name both
#         as candidates and the stick as the source, refuse an answer that is
#         not one of them, refuse a confirmation that is not the disk's own
#         name, and then write the one it was told to and leave the other as it
#         was. ----
NEWPASS=an-installed-machine
USERNAME=alice
USERPASS=alices-own-password
rm -f "$DIR/one.img" "$DIR/two.img"
dd if=/dev/zero of="$DIR/one.img" bs=1048576 count=256 2>/dev/null
dd if=/dev/zero of="$DIR/two.img" bs=1048576 count=256 2>/dev/null
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
    --timeout 900 --steptmo 420 --ok "INSTALLDONE" --log "$LOG3" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=" \
    "expect=is not one of the disks offered" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk9" \
    "expect=is not one of the disks offered" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk1" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=the name of the first account" "send=ROOT" \
    "expect=is not a name this system takes" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk1" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=different" \
    "expect=the two do not match" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk1" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=the name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=authorized keys" "send=" \
    "expect=grow the root" "send=no" \
    "expect=type the name of that disk" "send=yes" \
    "expect=nothing was written" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk1" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=the name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=authorized keys" "send=" \
    "expect=grow the root" "send=" \
    "expect=type the name of that disk" "send=/dev/disk1" \
    "expect=the image on the stick is the one the manifest describes" \
    "expect=what reached the disk is the image" \
    "expect=the table is the one the image carries" \
    "expect=the accounts are root and $USERNAME" \
    "expect=growing the root into the rest of the disk" \
    "expect=installed:" "send=echo INSTALL\"\"DONE" \
    "expect=INSTALLDONE" > "$LOG3.harness" 2>&1; then
    note "the installer refuses an answer and a confirmation, and writes the disk it was told: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG3.harness" >&2
    bad "the install onto the second of two disks"
fi
# The disk it was not told to write is as it was. The one it did write is not
# compared with the image, since the accounts were written into its root after
# the image reached it; that the image reached it whole is what the readback
# inside the installer says, and row 4 boots what is there.
if [ "$(shasum -a 256 "$DIR/one.img" | cut -d' ' -f1)" = "$BLANK" ]; then
    note "and the disk it was not told to write is as it was: OK"
else
    bad "the other disk was written"
fi
# The nodes the reread published are the image's four partitions.
if [ "$(grep -ac ' part 1 ' "$LOG3")" -ge 2 ]; then
    note "and the new table's partitions were published: OK"
else
    bad "the new table was not published"
fi

# ---- 3b: the same machine, the other disk. The disks are the same two in the
#          same order, and the answer names the first this time, so what
#          decides which disk is written is the name typed and not the
#          position. The one written in row 3 must be left as row 3 left it.
LOG3B="$DIR/install-other.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --qemu-arg=-drive --qemu-arg="file=$DIR/two.img,format=raw,if=ide,index=1,media=disk" \
    --timeout 900 --steptmo 420 --ok "OTHERDONE" --log "$LOG3B" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=the name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=authorized keys" "send=/root/.ssh/authorized_keys" \
    "expect=grow the root" "send=no" \
    "expect=type the name of that disk" "send=/dev/disk0" \
    "expect=the image on the stick is the one the manifest describes" \
    "expect=what reached the disk is the image" \
    "expect=installed:" "send=echo OTHER\"\"DONE" \
    "expect=OTHERDONE" > "$LOG3B.harness" 2>&1; then
    note "the other disk of the same machine takes the same install: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG3B.harness" >&2
    bad "the install onto the first of two disks"
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
    "send=echo INSTALLED\"\"UP" \
    "expect=INSTALLEDUP" > /dev/null 2>&1; then
    note "and the disk it wrote boots, refuses the image's own password and takes the new one: OK"
else
    bad "the boot of the installed disk"
fi

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
    "expect=# " "send=echo SSH\"\"READY" "expect=SSHREADY" wait=90 \
    > "$LOG6.harness" 2>&1 &
QPID=$!

# The three attempts, while that machine is up. Each says what it proves.
sleep 60
OUT=$(RACH2_PASSWORD="$NEWPASS" "$PTYDRIVE" --rows 40 --cols 100 --timeout 60 -- \
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o ConnectTimeout=20 -o PubkeyAuthentication=no \
    -o PreferredAuthentications=password -o NumberOfPasswordPrompts=3 -tt \
    -p "$PORT" root@localhost 'id -u; echo ROOT""PASSWORD' 2>&1 || true)
case "$OUT" in
    *ROOTPASSWORD*) note "root logs in over SSH with the password the install was given: OK" ;;
    *) bad "root's password login over SSH: $OUT" ;;
esac
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o ConnectTimeout=20 -o BatchMode=yes -o IdentitiesOnly=yes \
    -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
    -i "$DEMO_KEY" -p "$PORT" "$USERNAME@localhost" 'id -un; echo KEY""LOGIN' 2>&1 || true)
case "$OUT" in
    *KEYLOGIN*) note "and the account logs in with the keys the install was given: OK" ;;
    *) bad "the account's key login: $OUT" ;;
esac
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o ConnectTimeout=20 -o BatchMode=yes -o IdentitiesOnly=yes \
    -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
    -i "$DEMO_KEY" -p "$PORT" root@localhost 'echo DEMO""KEY' 2>&1 || true)
case "$OUT" in
    *DEMOKEY*) bad "the image's demonstration key still opens root on the installed machine" ;;
    *) note "and the image's own key no longer opens root: OK" ;;
esac
wait "$QPID" || bad "the installed machine did not stay up for the SSH attempts"

# ---- 7: a stick whose image is not the one its manifest describes. The
#         manifest's hash is changed on the host, one digit of it, which is
#         what a stick damaged in the writing or in a pocket looks like from
#         the machine's side. Nothing may be written. ----
cp "$TEST_STICK" "$DIR/bad-stick.img"
spoil_manifest "$DIR/bad-stick.img" || bad "the manifest on the copy could not be changed"
rm -f "$DIR/four.img"
dd if=/dev/zero of="$DIR/four.img" bs=1048576 count=256 2>/dev/null
LOG7="$DIR/badhash.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/bad-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/four.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 900 --steptmo 420 --ok "BADHASHDONE" --log "$LOG7" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-install" \
    "expect=which disk shall it be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=the name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=authorized keys" "send=" \
    "expect=grow the root" "send=no" \
    "expect=type the name of that disk" "send=/dev/disk0" \
    "expect=and the manifest says" \
    "expect=# " "send=echo BADHASH\"\"DONE" \
    "expect=BADHASHDONE" > "$LOG7.harness" 2>&1; then
    note "a stick whose image is not the one its manifest describes is refused: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOG7.harness" >&2
    bad "the refusal of a damaged stick"
fi
if [ "$(shasum -a 256 "$DIR/four.img" | cut -d' ' -f1)" = "$BLANK" ]; then
    note "and the disk it was going to write is as it was: OK"
else
    bad "the disk was written from a stick whose image did not match its manifest"
fi

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

# ---- 9: a machine whose disk has been destroyed, brought back through the
#         recovery menu. The first mebibyte of the disk installed in row 3b is
#         zeroed on the host, which takes its boot code, its table and the
#         start of its first partition; the stick is booted on it, and
#         `rach2-recover` puts the stick's own image back and reads it back.
#         The disk must then boot again. ----
dd if=/dev/zero of="$DIR/one.img" bs=1048576 count=1 conv=notrunc 2>/dev/null
LOG9="$DIR/recover.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 900 --steptmo 420 --ok "RECOVERDONE" --log "$LOG9" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=rach2-recover" \
    "expect=which?" "send=1" \
    "expect=/dev/disk0:" \
    "expect=which?" "send=3" \
    "expect=which disk shall be restored" "send=/dev/disk0" \
    "expect=the image to put on it" "send=" \
    "expect=type the name of that disk" "send=/dev/disk0" \
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

if [ "$fail" -ne 0 ]; then
    echo "installer-test: FAIL" >&2
    exit 1
fi
echo "installer-test: PASS"
