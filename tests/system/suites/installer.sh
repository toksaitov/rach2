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
#       [all|install|repair|resume|rehearse|recoverykey] [root-image]
#
# The first stick is the product's, carrying the sealed disk; the second
# carries a 65 MiB disk instead, so the rows that install do not each write a
# gibibyte and a half.
#

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
RUNUTIL="$REPO/build/tests/system/runutil"
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
# The product stick carries the sealed disk under the disk's own name, which
# says whether its recovery system was built with a key.
DISK_NAME=$(basename "$DISK")

mkdir -p "$DIR"

# A machine left running by an earlier run of this suite holds the images this
# one writes, and QEMU refuses to open a file another process has locked. Any
# such machine is this suite's own, since the paths are its own.
for left in $(pgrep -f "$DIR/" 2>/dev/null || true); do
    kill "$left" 2>/dev/null || true
    "$RUNUTIL" gone "$left" 30 || true
done

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

# ---- the recovery system's key ----
#
# The key an install places on the recovery partition is proved on the
# recovery system that partition boots, and that system's image must have no
# key of its own for the proof to be one. The images a run builds have this
# tree's key, since `tests/run` mints one, so the image these rows boot is
# staged here from the recovery manifest with the key's line taken out, which
# is the image a tree with no key builds, and it replaces the one the installed
# disk has. It is staged once a run, from this tree's programs.
RKEY_FILE="$DIR/recovery.key"
KEYLESS="$DIR/keyless/rach2-recovery.krffs"
keyless_made=""
keyless_image() {
    [ -z "$keyless_made" ] || return 0
    rm -rf "$DIR/keyless"
    mkdir -p "$DIR/keyless"
    sed '/^optional-file .* \/root\/\.ssh\/authorized_keys /d' \
        "$REPO/krffs/manifests/rach2-recovery.manifest" > "$DIR/keyless/rach2-recovery.manifest"
    if grep -q '^optional-file.*authorized_keys' "$DIR/keyless/rach2-recovery.manifest"; then
        bad "the recovery manifest's key line was not taken out"
        return 1
    fi
    if ! sh "$REPO/krffs/mkimage.sh" "$DIR/keyless/rach2-recovery.manifest" "$KEYLESS" 4 \
        > "$DIR/keyless/mkimage.log" 2>&1; then
        bad "the recovery image with no key was not staged; see $DIR/keyless/mkimage.log"
        return 1
    fi
    keyless_made=yes
}

# The first sector of the installed disk's recovery partition, its second
# entry.
recovery_start() { # recovery_start IMAGE
    od -An -tu4 -j $((446 + 16 + 8)) -N 4 "$1" | tr -d ' '
}

# The keyless image put on an installed disk's recovery partition in place of
# its own, and the recovery entry armed for the disk's next boot the way
# grub-editenv arms it: next_entry set and every other variable of the block
# kept.
recovery_boot_disk() { # recovery_boot_disk IMAGE
    at=$(recovery_start "$1")
    [ -n "$at" ] && [ "$at" -gt 0 ] || return 1
    mdel -i "$1@@$((at * 512))" ::/boot/recovery.krffs || return 1
    mcopy -i "$1@@$((at * 512))" "$KEYLESS" ::/boot/recovery.krffs || return 1
    env_at=$(grep -abo '# GRUB Environment Block' "$1" | head -1 | cut -d: -f1)
    [ -n "$env_at" ] || return 1
    {
        printf '# GRUB Environment Block\n'
        printf '# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
        dd if="$1" bs=1 skip="$env_at" count=1024 2>/dev/null |
            sed 's/#*$//' | grep -a -v -e '^#' -e '^$' -e '^next_entry='
        printf 'next_entry=recovery\n'
    } > "$1.env"
    n=$(wc -c < "$1.env" | tr -d ' ')
    dd if=/dev/zero bs=1 count=$((1024 - n)) 2>/dev/null | tr '\0' '#' >> "$1.env"
    dd if="$1.env" of="$1" bs=1 seek="$env_at" conv=notrunc 2>/dev/null || return 1
    rm -f "$1.env"
}

# ssh to the machine's port as root, with the one key given and nothing of
# the runner's own.
key_ssh() { # key_ssh KEY COMMAND
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$1" -p "$PORT" root@localhost "$2" 2>&1
}

# ---- 6b and 6c: the recovery system's key, placed and not. KEYED is a disk
#          installed with $RKEY_FILE's public half as the key for its
#          recovery system and UNKEYED one installed with none. Each is
#          copied, given the keyless image and booted into its recovery
#          system through its own menu. The keyed disk's recovery system must
#          say it took the key off its partition, let root in with that key
#          and no other, and leave the disk with no mount on it; the unkeyed
#          one must say SSH is off and answer nothing on the port. ----
recovery_key_rows() { # recovery_key_rows KEYED UNKEYED
    keyless_image || return 0
    RKEY_ON="$DIR/rkey-on.img"
    cp "$1" "$RKEY_ON" || { bad "row 6b: the keyed disk could not be copied"; return 0; }
    at=$(recovery_start "$RKEY_ON")
    placed=$(mtype -i "$RKEY_ON@@$((at * 512))" ::/boot/recovery_authorized_keys 2>/dev/null | tr -d '\r')
    if [ "$placed" = "$RKEY" ]; then
        note "row 6b: the install put the key it was given beside the recovery system: OK"
    else
        bad "row 6b: the recovery partition's key file holds '$placed', not the key given"
    fi
    recovery_boot_disk "$RKEY_ON" || { bad "row 6b: the recovery entry could not be armed"; return 0; }
    LOG6B="$DIR/rkey-on.log"
    rm -f "$KNOWN" "$LOG6B" "$LOG6B.harness"
    # init forks the console session before it starts the server, so the
    # prompt says nothing about the port. The harness's `tcp=` step connects
    # until the server's banner arrives, within its bound, and the token the
    # attempts wait for is typed after it; the harness goes on past a knock
    # that failed, and the first attempt then fails with what ssh printed.
    "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-drive --qemu-arg="file=$RKEY_ON,format=raw,if=ide,index=0,media=disk" \
        --net "user,id=net0,hostfwd=tcp::$PORT-:22" --device "rtl8139,netdev=net0" \
        --timeout 600 --steptmo 480 --ok "RKEYDONE" --log "$LOG6B" \
        "expect=recovery image" \
        "expect=# " "tcp=$PORT" "send=echo RKEY\"\"READY" "expect=RKEYREADY" "expect=RKEYDONE" \
        > "$LOG6B.harness" 2>&1 &
    QPID=$!
    if ! "$RUNUTIL" until "$LOG6B" "RKEYREADY" 300; then
        kill -TERM "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        bad "row 6b: the disk's recovery system never came up"
        return 0
    fi
    if grep -aq 'init: recovery key: taken from /dev/disk0p2' "$LOG6B" &&
        grep -aq "no key was built into this recovery image, and its recovery partition's key turns SSH on" "$LOG6B"; then
        note "row 6b: the recovery system took the key off its own partition: OK"
    else
        bad "row 6b: the recovery system did not say it took the key off its partition"
    fi
    OUT=$(key_ssh "$RKEY_FILE" 'id -u; grep "^identity 0 " /proc/disk; echo RECOVERY""KEY' || true)
    case "$OUT" in
        *RECOVERYKEY*) note "row 6b: root logs in to the recovery system with the key the install placed: OK" ;;
        *) bad "row 6b: the placed key's login: $OUT" ;;
    esac
    case "$OUT" in
        *"identity 0 "*" free "*) note "row 6b: and the disk has no mount left on it: OK" ;;
        *) bad "row 6b: the disk is not free after the key was read: $OUT" ;;
    esac
    OUT=$(key_ssh "$DEMO_KEY" 'echo DEMO""KEY' || true)
    case "$OUT" in
        *DEMOKEY*) bad "row 6b: a key the install was not given opens the recovery system" ;;
        *"Permission denied"*) note "row 6b: and a key it was not given is refused: OK" ;;
        *) bad "row 6b: the other key's attempt neither ran nor was refused: $OUT" ;;
    esac
    OUT=$(key_ssh "$RKEY_FILE" 'echo TOLD; echo RKEY""DONE > /dev/console' || true)
    case "$OUT" in
        *TOLD*) wait "$QPID" || bad "row 6b: the recovery system did not stay up for the attempts" ;;
        *)
            kill -TERM "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
            bad "row 6b: the recovery system could not be told the attempts are over: $OUT"
            ;;
    esac

    RKEY_OFF="$DIR/rkey-off.img"
    cp "$2" "$RKEY_OFF" || { bad "row 6c: the unkeyed disk could not be copied"; return 0; }
    at=$(recovery_start "$RKEY_OFF")
    if mtype -i "$RKEY_OFF@@$((at * 512))" ::/boot/recovery_authorized_keys > /dev/null 2>&1; then
        bad "row 6c: a disk installed with no key for its recovery system has one"
    else
        note "row 6c: a disk installed with no key for its recovery system has none: OK"
    fi
    recovery_boot_disk "$RKEY_OFF" || { bad "row 6c: the recovery entry could not be armed"; return 0; }
    LOG6C="$DIR/rkey-off.log"
    rm -f "$KNOWN" "$LOG6C" "$LOG6C.harness" "$DIR/rkey-off.barrier"
    # Each turn of init's loop forks a console session if none is running,
    # starts the server if root has an authorized key, and then reaps a
    # child. A session forked in a turn comes before that turn's test for a
    # key, so two are ended, and the token is typed at the third, which init
    # forks only after the turn that forked the second has made its test.
    # init prints `init: sshd: started` in the turn it forks the server,
    # before it forks another session, so the log up to the token says
    # whether either turn started one, however late the server's own exec or
    # listen comes; the port is probed after it.
    "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-drive --qemu-arg="file=$RKEY_OFF,format=raw,if=ide,index=0,media=disk" \
        --net "user,id=net0,hostfwd=tcp::$PORT-:22" --device "rtl8139,netdev=net0" \
        --timeout 600 --steptmo 480 --ok "NOSERVERDONE" --log "$LOG6C" \
        "expect=recovery image" \
        "expect=# " "send=exit" "expect=# " "send=exit" "expect=# " \
        "send=echo NOSERVER\"\"READY" "expect=NOSERVERREADY" \
        "barrier=$DIR/rkey-off.barrier" \
        "send=echo NOSERVER\"\"DONE" "expect=NOSERVERDONE" > "$LOG6C.harness" 2>&1 &
    QPID=$!
    if ! "$RUNUTIL" until "$LOG6C" "NOSERVERREADY" 300; then
        kill -TERM "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        bad "row 6c: the disk's recovery system never came up"
        return 0
    fi
    if grep -aq 'init: no key was built into this recovery image, so SSH is off' "$LOG6C" &&
        ! grep -aq 'init: recovery key: taken from' "$LOG6C"; then
        note "row 6c: the recovery system says SSH is off: OK"
    else
        bad "row 6c: the recovery system of a disk with no key did not say SSH is off"
    fi
    started=0
    grep -aq 'init: sshd: started' "$LOG6C" || started=$?
    case $started in
        0) bad "row 6c: init started an SSH server on a recovery system with no key: $(grep -a 'init: sshd: started' "$LOG6C" | head -1)" ;;
        1) note "row 6c: init started no SSH server by its second test for a key: OK" ;;
        *) bad "row 6c: the log could not be read for init's record of the server, status $started" ;;
    esac
    # What answers the port. With -v, ssh reports a server's identification
    # line as `Remote protocol version`, whatever follows it, and any server
    # fails the row. With none there, QEMU closes the forwarded connection
    # when the guest refuses it, and ssh says it was closed or reset before
    # a banner; that, or a connection refused, is the pass. Anything else, a
    # timeout or an ssh that did not run, proves nothing.
    OUT=$(ssh -v -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$RKEY_FILE" -p "$PORT" root@localhost 'echo NO""DOOR' 2>&1 || true)
    case "$OUT" in
        *"Remote protocol version"* | *NODOOR* | *"Permission denied"*)
            bad "row 6c: an SSH server answered on a recovery system with no key: $OUT"
            ;;
        *"Connection closed by"* | *"Connection reset by"* | *"connect to host localhost port $PORT: Connection refused"*)
            note "row 6c: and nothing answers SSH: OK"
            ;;
        *) bad "row 6c: the probe ended in neither a banner nor a closed connection, so it proves nothing: $OUT" ;;
    esac
    touch "$DIR/rkey-off.barrier"
    wait "$QPID" || bad "row 6c: the recovery system did not finish its boot"
    rm -f "$RKEY_ON" "$RKEY_OFF"
}

if [ "$PART" = "all" ] || [ "$PART" = "install" ] || [ "$PART" = "recoverykey" ]; then
rm -f "$RKEY_FILE" "$RKEY_FILE.pub"
ssh-keygen -q -t ed25519 -N '' -C recovery -f "$RKEY_FILE"
RKEY=$(cat "$RKEY_FILE.pub")
fi

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
    "expect=recovery image" \
    "expect=# " "send=mkdir -p /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=cat /mnt/rach2-install.manifest" \
    "expect=sha256 " \
    "send=set -- \`ls -l /mnt/$DISK_NAME\`; echo SIZE\"\":\$5" \
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

# The stick the machine booted from is its own disk, whole, with three
# partitions: the ISO, the volume and the ISO's EFI boot image.
if grep -aq "usbdisk: disk0: mbr: $((BYTES / 1048576)) MiB, 3 partitions" "$LOG"; then
    note "and the stick it booted from is its disk0 with three partitions: OK"
else
    bad "the stick was not published whole with three partitions"
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
    "expect=recovery image" \
    "expect=# " "send=mkdir -p /mnt && mount -t exfat /dev/disk1p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=dd if=/mnt/$DISK_NAME bs=1048576 count=32 2>/dev/null | sha256sum; echo SUM\"\"ONE" \
    "expect=SUMONE" \
    "send=dd if=/mnt/$DISK_NAME bs=1048576 count=32 2>/dev/null | reflash -p /dev/disk0 0 33554432" \
    "expect=wrote 33554432 bytes" \
    "send=reflash --verify /dev/disk0 0 33554432; echo VERIFY\"\"ONE" \
    "expect=VERIFYONE" \
    "send=dd if=/mnt/$DISK_NAME bs=1048576 skip=40 count=2 2>/dev/null | sha256sum; echo SUM\"\"TWO" \
    "expect=SUMTWO" \
    "send=dd if=/mnt/$DISK_NAME bs=1048576 skip=40 count=2 2>/dev/null | reflash /dev/disk0 33554432 2097152" \
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
    "expect=recovery image" \
    "expect=# " "send=echo '-----BEGIN OPENSSH PRIVATE KEY-----' > /tmp/private.key; printf '\\043 a comment\\n' > /tmp/comments.key" \
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
    "expect=key for the recovery system" "send=/no/such/key" \
    "expect=is not a file" \
    "expect=key for the recovery system" "send=/tmp/private.key" \
    "expect=holds a private key" \
    "expect=key for the recovery system" "send=/tmp/comments.key" \
    "expect=has no line that is a key" \
    "expect=key for the recovery system" "send=" \
    "expect=row the root" "send=no" \
    "expect=Check this before you go on" \
    "expect=Key for the recovery system: none" \
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
    "expect=key for the recovery system" "send=" \
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
    "expect=recovery image" \
    "expect=# " "send=echo '$RKEY' > /tmp/recovery.pub" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=/root/.ssh/authorized_keys" \
    "expect=key for the recovery system" "send=/tmp/recovery.pub" \
    "expect=row the root" "send=no" \
    "expect=Key for the recovery system:" \
    "expect=to go on, or stop to stop" "send=/dev/disk0" \
    "expect=the image on the stick is the one the manifest describes" \
    "expect=what reached the disk is the image" \
    "expect=takes the key in /tmp/recovery.pub" \
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
# third entry, which begins at 446 + 32.
GROWN=$(dd if="$DIR/two.img" bs=1 skip=490 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
KEPT=$(dd if="$DIR/one.img" bs=1 skip=490 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
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
    "send=mkdir -p /r && mount -t vfat /dev/disk0p2 /r && cmp /r/rach2-config-a/etc/shadow /etc/shadow && grep -c ^$USERNAME: /r/rach2-config-a/etc/passwd; umount /r" \
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
    "expect=recovery image" \
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
#         refuses root a password. The machine stays up until the attempts
#         are over: the last of them writes a token on the machine's console,
#         which ends the harness's run, so the wait is for that token and not
#         for a number of seconds, which a set's factor would stretch past the
#         runner's idle limit. ----
# The log is removed first: the wait below would take the token from a
# previous run's log before the harness truncates it.
rm -f "$KNOWN"
LOG6="$DIR/ssh.log"
rm -f "$LOG6" "$LOG6.harness"
"$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --net "user,id=net0,hostfwd=tcp::$PORT-:22" --device "rtl8139,netdev=net0" \
    --timeout 600 --steptmo 480 --ok "SSHDONE" --log "$LOG6" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "tcp=$PORT" "send=echo SSH\"\"READY" "expect=SSHREADY" "expect=SSHDONE" \
    > "$LOG6.harness" 2>&1 &
QPID=$!

# The three attempts, once the machine has said it is up, which it does after
# the harness's `tcp=` step has read the server's banner: init forks the login
# before it starts the server. Each attempt says what it proves; with no
# machine up they fail with what ssh printed.
if ! "$RUNUTIL" until "$LOG6" "SSHREADY" 120; then
    kill -TERM "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    bad "the machine never said it was up for the SSH attempts"
fi
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
# The attempts are over: root answers this session first and then writes
# the token on the console, which ends the harness's run, so the reply is
# in hand before the machine can be stopped. The run's budget of 480 s a
# step covers the four connections of this row together. A machine that
# cannot be told is stopped, harness and machine, so the failure is
# reported now and not at the run's limit.
OUT=$(RACH2_PASSWORD="$NEWPASS" "$PTYDRIVE" --rows 40 --cols 100 --timeout 60 -- \
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o "ConnectTimeout=$(scaled 20)" -o PubkeyAuthentication=no \
    -o PreferredAuthentications=password -o NumberOfPasswordPrompts=3 -tt \
    -p "$PORT" root@localhost 'echo TOLD; echo SSH""DONE > /dev/console' 2>&1 || true)
case "$OUT" in
    *TOLD*) wait "$QPID" || bad "the installed machine did not stay up for the SSH attempts" ;;
    *)
        kill -TERM "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        bad "the installed machine could not be told the attempts are over: $OUT"
        ;;
esac

mark "row 6"
recovery_key_rows "$DIR/one.img" "$DIR/two.img"
mark "rows 6b and 6c"
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
    "expect=recovery image" \
    "expect=# " "send=rach2-install" \
    "expect=disk shall Rach2 be written to" "send=/dev/disk0" \
    "expect=the password for root" "send=$NEWPASS" \
    "expect=again" "send=$NEWPASS" \
    "expect=The name of the first account" "send=$USERNAME" \
    "expect=the password for $USERNAME" "send=$USERPASS" \
    "expect=again" "send=$USERPASS" \
    "expect=uthorized keys" "send=" \
    "expect=key for the recovery system" "send=" \
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
    "expect=recovery image" \
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

# The recovery key rows alone, for tests/system/recovery_key_mutate.sh: two
# installs from the test stick onto blank disks, one given the key for the
# recovery system and one given none, and rows 6b and 6c on them.
if [ "$PART" = "recoverykey" ]; then
MARK=$(date +%s)
NEWPASS=an-installed-machine
USERNAME=alice
USERPASS=alices-own-password
cp "$TEST_STICK" "$DIR/test-stick.img"
quick_install() { # quick_install DISK ANSWER: the install, with ANSWER for the recovery key
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1048576 count=0 seek=128 2>/dev/null
    "$HARNESS" --bios \
        --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
        --qemu-arg=-drive --qemu-arg="file=$1,format=raw,if=ide,index=0,media=disk" \
        --timeout 3600 --steptmo 1800 --ok "QUICKDONE" --log "$1.log" \
        "expect=recovery image" \
        "expect=# " "send=echo '$RKEY' > /tmp/recovery.pub" \
        "expect=# " "send=rach2-install" \
        "expect=disk shall Rach2 be written to" "send=/dev/disk0" \
        "expect=the password for root" "send=$NEWPASS" \
        "expect=again" "send=$NEWPASS" \
        "expect=The name of the first account" "send=$USERNAME" \
        "expect=the password for $USERNAME" "send=$USERPASS" \
        "expect=again" "send=$USERPASS" \
        "expect=uthorized keys" "send=" \
        "expect=key for the recovery system" "send=$2" \
        "expect=row the root" "send=no" \
        "expect=to go on, or stop to stop" "send=/dev/disk0" \
        "expect=installed:" "send=echo QUICK\"\"DONE" \
        "expect=QUICKDONE" > "$1.harness" 2>&1
}
if quick_install "$DIR/rk-keyed.img" /tmp/recovery.pub &&
    quick_install "$DIR/rk-unkeyed.img" ""; then
    note "the two installs for the recovery key rows: OK"
    mark "installs"
    recovery_key_rows "$DIR/rk-keyed.img" "$DIR/rk-unkeyed.img"
    mark "rows 6b and 6c"
else
    bad "the installs for the recovery key rows"
fi
rm -f "$DIR/rk-keyed.img" "$DIR/rk-unkeyed.img"
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
    "expect=recovery image" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=dd if=/m/$(basename "$TEST_DISK") bs=1048576 count=96 2>/dev/null | reflash -p /dev/disk0 0 100663296" \
    "expect=NEVERSEEN" > /dev/null 2>&1 &
CUTPID=$!
saw_progress=no
if "$RUNUTIL" until "$DIR/cutoff.log" 'reflash: 16777216 of ' 300; then
    saw_progress=yes
fi
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
for left in $(pgrep -f "file=$DIR/cut.img" 2>/dev/null || true); do
    "$RUNUTIL" gone "$left" 60 || true
done
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
    "expect=recovery image" \
    "expect=# " "send=rach2-recover" \
    "expect=which?" "send=1" \
    "expect=/dev/disk0:" \
    "expect=which?" "send=3" \
    "expect=which disk shall be restored" "send=/dev/disk0" \
    "expect=the image to put on it" "send=" \
    "expect=to go on, anything else to stop" "send=/dev/disk0" \
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
#          as an install removes them, an account's home is made with a file of
#          8 MiB in it, more than the recovery system's root has room for, and a
#          file outside everything the update keeps is marked, and all of it is
#          synced, since ext2 writes back and the harness stops the machine
#          without a shutdown. The update must then leave the password, the
#          absent keys and the home as they are, and must replace the marked
#          file with the image's, which is what says the root was written at
#          all. An archive in the old layout, whose group file has one line the
#          root's lacks, is put on the recovery partition; the update must name
#          that difference and keep the root's file.
LOG11="$DIR/changed.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "CHANGEDDONE" --log "$LOG11" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=echo root:$NEWPASS | chpasswd && echo PASSWORD\"\"SET" \
    "expect=PASSWORDSET" \
    "send=rm -rf /root/.ssh && mkdir -p /home/$USERNAME && chown 1000:1000 /home/$USERNAME" \
    "send=echo this-machines-own > /home/$USERNAME/mine && chown 1000:1000 /home/$USERNAME/mine" \
    "send=dd if=/dev/zero of=/home/$USERNAME/big bs=1048576 count=8 2>/dev/null" \
    "send=echo a-marked-motd > /etc/motd && sync && echo CHANGED\"\"DONE" \
    "expect=CHANGEDDONE" \
    "send=mkdir -p /r && mount -t vfat /dev/disk0p2 /r && mkdir -p /r/rach2-config/etc && cp /etc/passwd /etc/shadow /r/rach2-config/etc/ && { cat /etc/group; echo tampered:x:99:; } > /r/rach2-config/etc/group && umount /r && echo ARCHIVE\"\"TAMPERED" \
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
    "expect=recovery image" \
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
    "expect=# " "send=cat /etc/motd" "expect=The log of this boot is in /dev/bootlog" \
    "send=test ! -d /root/.ssh && echo KEYS\"\"GONE" "expect=KEYSGONE" \
    "send=cat /home/$USERNAME/mine; stat -c %u /home/$USERNAME/mine" \
    "expect=this-machines-own" "expect=1000" \
    "send=stat -c BIG=%s /home/$USERNAME/big" "expect=BIG=8388608" \
    "send=echo UPDATED\"\"UP" \
    "expect=UPDATEDUP" > /dev/null 2>&1; then
    note "and it takes the password it had, keeps the home, its owner and its file of 8 MiB, \
and has the image's own motd where the marked one was: OK"
else
    bad "the boot of the updated machine"
fi

mark "row 11"
# ---- 14: the root damaged, and the machine updated from the archive: the
#          superblock zeroed so the root does not mount, then the update
#          takes the accounts from the archive the last update refreshed and
#          writes the image into the last ext2 entry of the table. The
#          archive keeps SSH keys but no other home files. ----
LOG14="$DIR/archive-update.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "ARCHIVEDONE" --log "$LOG14" \
    "expect=recovery image" \
    "expect=# " "send=dd if=/dev/zero of=/dev/disk0p3 bs=1024 seek=1 count=1 && echo DAMA\"\"GED" \
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
    "send=stat -c %u:%g:%a /home/test/.ssh /home/test/.ssh/authorized_keys" \
    "expect=1000:1000:700" "expect=1000:1000:600" \
    "send=bootctl commit && echo ARCHIVE\"\"UP" \
    "expect=ARCHIVEUP" > /dev/null 2>&1; then
    note "and it takes the password the archive held, the archive refreshed by the last update and not the tampered one, and loses only the ordinary home files absent from the archive: OK"
else
    bad "the boot of the machine updated from the archive"
fi

mark "row 14"
fi

if [ "$PART" = "all" ] || [ "$PART" = "repair" ] || [ "$PART" = "resume" ]; then
# The focused proof needs only a bootable disk and the recovery rows.
if [ "$PART" = "resume" ]; then
    cp "$TEST_STICK" "$DIR/test-stick.img"
    cp "$TEST_DISK" "$DIR/one.img"
    NEWPASS=$PASSWORD
fi

# ---- 16: an update stopped after the root write. Distinct authorized_keys
#          contents distinguish this machine's keys from the image's. The
#          hashes come from the installed root before the archive is marked.
PUBKEY=$(ssh-keygen -y -f "$DEMO_KEY")
for account in worker upper lower; do
    rm -f "$DIR/$account.key" "$DIR/$account.key.pub"
    ssh-keygen -q -t ed25519 -N '' -C "$account" -f "$DIR/$account.key"
done
WORKKEY=$(cat "$DIR/worker.key.pub")
UPPERKEY=$(cat "$DIR/upper.key.pub")
LOWERKEY=$(cat "$DIR/lower.key.pub")
LOGKEYS="$DIR/before-resume.log"
if "$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 300 --steptmo 120 --ok "KEYSSAVED" --log "$LOGKEYS" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=mkdir -p /root/.ssh && echo '$PUBKEY' > /root/.ssh/authorized_keys" \
    "send=(test -s /etc/dropbear/dropbear_ed25519_host_key || dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key) && echo HOST\"\"READY" \
    "expect=HOSTREADY" \
    "send=mkdir -p /etc/ssh && echo 'openssh host key' > /etc/ssh/ssh_host_ed25519_key && chmod 600 /etc/ssh/ssh_host_ed25519_key" \
    "send=echo '# machine root' >> /root/.ssh/authorized_keys; echo '# machine test' >> /home/test/.ssh/authorized_keys" \
    "send=chmod 700 /root/.ssh /home/test/.ssh; chmod 600 /root/.ssh/authorized_keys /home/test/.ssh/authorized_keys" \
    "send=chown -R 0:0 /root/.ssh; chown -R 1000:1000 /home/test/.ssh" \
    "send=echo worker:x:2001:2001::/home/work:/bin/sh >> /etc/passwd; echo Alice:x:2002:2002::/home/Alice:/bin/sh >> /etc/passwd" \
    "send=echo alice:x:2003:2003::/home/alice:/bin/sh >> /etc/passwd; mkdir -p /home/work/.ssh /home/Alice/.ssh /home/alice/.ssh" \
    "send=echo '$WORKKEY' > /home/work/.ssh/authorized_keys" \
    "send=echo '$UPPERKEY' > /home/Alice/.ssh/authorized_keys" \
    "send=echo '$LOWERKEY' > /home/alice/.ssh/authorized_keys" \
    "send=echo service:x:2004:2004::/srv/alice:/bin/sh >> /etc/passwd; echo empty:x:2005:2005::/srv/empty:/bin/sh >> /etc/passwd" \
    "send=mkdir -p /srv/alice/.ssh; echo '$WORKKEY' > /srv/alice/.ssh/authorized_keys; chown -R 2004:2004 /srv/alice; chmod 750 /srv/alice/.ssh; chmod 640 /srv/alice/.ssh/authorized_keys" \
    "send=sha256sum /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys /home/test/.ssh/authorized_keys | sed s/^/KEYHASH:/" \
    "send=sha256sum /home/work/.ssh/authorized_keys /home/Alice/.ssh/authorized_keys /home/alice/.ssh/authorized_keys /srv/alice/.ssh/authorized_keys | sed s/^/KEYHASH:/" \
    "send=sync; echo KEYS\"\"SAVED" "expect=KEYSSAVED" > "$LOGKEYS.harness" 2>&1; then
    note "the installed machine's keys are recorded before the interrupted update: OK"
else
    bad "recording the machine's keys before the interrupted update"
    exit 1
fi
grep -a '^KEYHASH:[0-9a-f]' "$LOGKEYS" | tr -d '\r' > "$DIR/keys.before"
[ "$(wc -l < "$DIR/keys.before")" -ge 6 ] || { bad "the original key hashes are missing"; exit 1; }

# The payload has keys at both nonstandard homes, including the account
# whose machine configuration has no keys. Both must replace the image's.
cp "$INSTALLER_EXT2" "$DIR/key-root.ext2"
INSTALLER_EXT2="$DIR/key-root.ext2"
echo image-key > "$DIR/image-key"
for home in alice empty; do
    debugfs -w -R 'mkdir /srv' "$INSTALLER_EXT2" > /dev/null 2>&1
    debugfs -w -R "mkdir /srv/$home" "$INSTALLER_EXT2" > /dev/null 2>&1
    debugfs -w -R "mkdir /srv/$home/.ssh" "$INSTALLER_EXT2" > /dev/null 2>&1
    debugfs -w -R "write $DIR/image-key /srv/$home/.ssh/authorized_keys" "$INSTALLER_EXT2" > /dev/null 2>&1
    debugfs -R "cat /srv/$home/.ssh/authorized_keys" "$INSTALLER_EXT2" 2>/dev/null | grep -qx image-key
done
PAYLOAD_START=$(od -An -tu4 -j $((446 + 16 + 8)) -N 4 "$DIR/test-stick.img" | tr -d ' ')
PAYLOAD_SIZE=$(od -An -tu4 -j $((446 + 16 + 12)) -N 4 "$DIR/test-stick.img" | tr -d ' ')
"$REPO/build/tests/host/mkexfatfixture" --make "$DIR/key-source.vol" \
    --sectors "$PAYLOAD_SIZE" --offset "$PAYLOAD_START" \
    --add "$(basename "$INSTALLER_EXT2")=$INSTALLER_EXT2" > /dev/null
dd if="$DIR/key-source.vol" of="$DIR/test-stick.img" bs=512 seek="$PAYLOAD_START" conv=notrunc 2>/dev/null

LOGSAVE="$DIR/archive-keys.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "KEYSARCHIVED" --log "$LOGSAVE" \
    "expect=recovery image" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=mkdir -p /r /a; mount -t ext2 /dev/disk0p3 /r && mount -t vfat /dev/disk0p2 /a" \
    "send=rm -rf /a/rach2-config /a/rach2-config.new /a/rach2-config.old /a/rach2-config-a /a/rach2-config-b" \
    "send=mkdir -p /a/rach2-config-a/etc && cp /r/etc/passwd /r/etc/group /r/etc/shadow /a/rach2-config-a/etc/" \
    "send=test -f /a/rach2-config-a/etc/passwd && test -f /a/rach2-config-a/etc/group && test -f /a/rach2-config-a/etc/shadow && test ! -e /a/rach2-config-a/generation && echo HALF\"\"READY" "expect=HALFREADY" \
    "send=echo stopped > /a/rach2-config-a/update-in-progress && umount /a && umount /r && echo HALF\"\"PLANTED" "expect=HALFPLANTED" \
    "send=mkdir -p /r; mount -t ext2 /dev/disk0p3 /r; cp /r/srv/alice/.ssh/authorized_keys /r/srv/alice/.ssh/AUTHORIZED_KEYS; umount /r" \
    "send=sha256sum /dev/disk0p3 > /tmp/root.before; rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0; echo COLLISION\"\"_STATUS=\$?" \
    "expect=COLLISION_STATUS=" \
    "send=sha256sum /dev/disk0p3 > /tmp/root.after; cmp -s /tmp/root.before /tmp/root.after; echo ROOT\"\"SAME=\$?" "expect=ROOTSAME=" \
    "send=mount -t ext2 /dev/disk0p3 /r; rm /r/srv/alice/.ssh/AUTHORIZED_KEYS; umount /r" \
    "send=rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0; echo UPDATE\"\"STATUS=\$?" \
    "expect=UPDATESTATUS=" \
    "send=mount -t ext2 /dev/disk0p3 /r; stat -c STAT=%u:%g:%a /r/srv/alice/.ssh /r/srv/alice/.ssh/authorized_keys" \
    "send=grep -qxF '$WORKKEY' /r/srv/alice/.ssh/authorized_keys; echo ALICE\"\"KEY=\$?; test -e /r/srv/empty/.ssh; echo EMPTY\"\"SSH=\$?; umount /r; echo CHECKS\"\"DONE" \
    "expect=CHECKSDONE" \
    "send=umount /m; echo KEYS\"\"ARCHIVED" "expect=KEYSARCHIVED" > "$LOGSAVE.harness" 2>&1; then
    :
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOGSAVE.harness" >&2
    bad "row archive-keys: the normal update's boot did not finish"
    exit 1
fi
# Every check above prints its answer, so a fault shows here at once instead
# of holding the boot to its step limit.
saved_value() { grep -a "^$1=" "$LOGSAVE" | tail -1 | tr -d '\r' | cut -d= -f2; }
saved_ok=1
if [ "$(saved_value COLLISION_STATUS)" = 1 ] &&
    grep -aq "ssh key names collide on FAT32: .*AUTHORIZED_KEYS and .*authorized_keys" "$LOGSAVE" &&
    [ "$(saved_value ROOTSAME)" = 0 ]; then
    note "row archive-keys: two key names that differ only in case stop the update before the root is written: OK"
else
    bad "row archive-keys: two key names that differ only in case were not refused before the root was written"
    saved_ok=0
fi
if [ "$(saved_value UPDATESTATUS)" = 0 ] && grep -aq "copying this machine's accounts" "$LOGSAVE" &&
    ! grep -aq "taking the configuration saved" "$LOGSAVE"; then
    note "row archive-keys: a half-written slot A is ignored and the update takes the root's configuration: OK"
else
    bad "row archive-keys: the update took a half-written slot or did not finish"
    saved_ok=0
fi
if grep -aq '^STAT=2004:2004:750' "$LOGSAVE" && grep -aq '^STAT=2004:2004:640' "$LOGSAVE" &&
    [ "$(saved_value ALICEKEY)" = 0 ]; then
    note "row archive-keys: the keys of a home outside /home are kept with their owner and modes: OK"
else
    bad "row archive-keys: the keys of a home outside /home were not kept"
    saved_ok=0
fi
if [ "$(saved_value EMPTYSSH)" = 1 ]; then
    note "row archive-keys: the image's key is removed from a home outside /home that had none: OK"
else
    bad "row archive-keys: an image key survived in a home outside /home"
    saved_ok=0
fi
if [ "$saved_ok" = 1 ]; then
    note "the machine's configuration is archived by a normal update: OK"
else
    bad "archiving the machine's configuration before the interrupted update"
    exit 1
fi

REC_START=$(od -An -tu4 -j $((446 + 1 * 16 + 8)) -N 4 "$DIR/one.img" | tr -d ' ')
[ -n "$REC_START" ] && [ "$REC_START" -gt 0 ] || bad "the recovery partition's start could not be read"
rm -rf "$DIR/saved-archive"
mkdir -p "$DIR/saved-archive"
mcopy -s -i "$DIR/one.img@@$((REC_START * 512))" '::rach2-config-a/*' "$DIR/saved-archive/"
rm "$DIR/saved-archive/generation"
mdeltree -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config-a
mdeltree -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config-b 2>/dev/null || true
mmd -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config
mcopy -s -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/saved-archive/"* ::rach2-config/
mcopy -n -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config/etc/passwd "$DIR/archived-passwd" ||
    bad "the archived passwd could not be read off the recovery partition"
echo "extra:x:1001:1001::/home/extra:/bin/sh" >> "$DIR/archived-passwd"
mmd -D s -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config/keys 2>/dev/null || true
mmd -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config/keys/2999 ||
    { bad "the unowned archived key directory could not be made"; exit 1; }
printf '%s\n' "$PUBKEY" "# orphan-uid-2999" > "$DIR/orphan-key"
mcopy -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/orphan-key" ::rach2-config/keys/2999/authorized_keys ||
    { bad "the unowned archived key could not be written"; exit 1; }
echo "$(basename "$INSTALLER_EXT2") stopped" > "$DIR/update-in-progress"
ROOT3_START=$(od -An -tu4 -j $((446 + 2 * 16 + 8)) -N 4 "$DIR/one.img" | tr -d ' ')
[ -n "$ROOT3_START" ] && [ "$ROOT3_START" -gt 0 ] || bad "the root partition's start could not be read"
dd if="$INSTALLER_EXT2" of="$DIR/one.img" bs=512 seek="$ROOT3_START" conv=notrunc 2>/dev/null ||
    bad "the image could not be written onto the root, as the stopped update left it"
mcopy -o -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/archived-passwd" ::rach2-config/etc/passwd ||
    bad "the archived passwd could not be written back to the recovery partition"
mcopy -o -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/update-in-progress" ::rach2-config/update-in-progress ||
    bad "the mark could not be planted on the recovery partition"
# A has the account files and mark but no generation. Clearing this
# unfinished slot must leave the complete legacy archive available.
mmd -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config-a ::rach2-config-a/etc
for account_file in passwd group shadow; do
    mcopy -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/saved-archive/etc/$account_file" "::rach2-config-a/etc/$account_file"
done
mcopy -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/update-in-progress" ::rach2-config-a/update-in-progress
LOGSTOP="$DIR/archive-stop.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "ARCHIVESTOPDONE" --log "$LOGSTOP" \
    "expect=recovery image" \
    "expect=# " "send=mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED" \
    "expect=MOUNTED" \
    "send=mkdir -p /r; mount -t vfat /dev/disk0p2 /r; cp -R /r/rach2-config /tmp/legacy-before; umount /r" \
    "send=sed 's|^    mkdir -p \"\$next_archive/etc/rach2\" |    sync; exit 99; mkdir -p \"\$next_archive/etc/rach2\" |' /bin/rach2-install > /tmp/stop-install" \
    "send=sh /tmp/stop-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0; echo STOP_STATUS=\$?" "expect=STOP_STATUS=99" \
    "send=mount -t vfat /dev/disk0p2 /r; diff -r /tmp/legacy-before /r/rach2-config && test ! -e /r/rach2-config/generation && echo CHOSEN\"\"WHOLE; umount /r; echo CHOSEN\"\"CHECKED" "expect=CHOSENCHECKED" \
    "send=rach2-install --update /m/$(basename "$INSTALLER_EXT2") /dev/disk0" \
    "expect=before the root is written" \
    "expect=what reached the root is the image" \
    "expect=the configuration is archived on" \
    "expect=updated:" "send=mkdir -p /r && mount -t vfat /dev/disk0p2 /r && echo RECOVERY\"\"MOUNTED" "expect=RECOVERYMOUNTED" \
    "send=grep -c ^extra: /r/rach2-config-a/etc/passwd | sed s/^/EXTRA=/" \
    "send=test ! -e /r/rach2-config && test ! -e /r/rach2-config.new && test ! -e /r/rach2-config.old && echo LEGACY\"\"GONE" \
    "send=a=\$(cat /r/rach2-config-a/generation); b=\$(cat /r/rach2-config-b/generation); test \"\$a\" -gt \"\$b\" && test ! -e /r/rach2-config-a/update-in-progress && echo ARCHIVE\"\"CURRENT" \
    "send=umount /r; umount /m; echo ARCHIVESTOP\"\"DONE" \
    "expect=ARCHIVESTOPDONE" > "$LOGSTOP.harness" 2>&1 &&
    grep -aq "taking the configuration saved on the recovery partition" "$LOGSTOP" &&
    grep -aq '^CHOSENWHOLE' "$LOGSTOP" && grep -aq '^LEGACYGONE' "$LOGSTOP" &&
    grep -aq '^EXTRA=1' "$LOGSTOP" && grep -aq '^ARCHIVECURRENT' "$LOGSTOP"; then
    note "row archive-stop: the complete legacy archive supplies the machine configuration: OK"
else
    sed -n 's/^HARNESS: /installer-test: /p' "$LOGSTOP.harness" >&2
    bad "row archive-stop: the retry did not recover the complete legacy archive"
    exit 1
fi

# Repeat the root-write stop with the newest archive in slot A.
mmd -D s -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config-a/keys 2>/dev/null || true
mmd -i "$DIR/one.img@@$((REC_START * 512))" ::rach2-config-a/keys/2999 ||
    { bad "the orphan directory could not be replanted for row 16"; exit 1; }
mcopy -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/orphan-key" ::rach2-config-a/keys/2999/authorized_keys ||
    { bad "the orphan key could not be replanted for row 16"; exit 1; }
dd if="$INSTALLER_EXT2" of="$DIR/one.img" bs=512 seek="$ROOT3_START" conv=notrunc 2>/dev/null ||
    { bad "the image could not be written for row 16"; exit 1; }
mcopy -o -i "$DIR/one.img@@$((REC_START * 512))" "$DIR/update-in-progress" ::rach2-config-a/update-in-progress ||
    { bad "the mark could not be planted for row 16"; exit 1; }
LOG16="$DIR/stopped-update.log"
if "$HARNESS" --bios \
    --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
    --drive "if=none,id=stick,format=raw,file=$DIR/test-stick.img" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --timeout 3600 --steptmo 1800 --ok "STOPPEDDONE" --log "$LOG16" \
    "expect=recovery image" \
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
rm -f "$LOG17" "$KNOWN"
"$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-drive --qemu-arg="file=$DIR/one.img,format=raw,if=ide,index=0,media=disk" \
    --net "user,id=net0,hostfwd=tcp::$PORT-:22" --device "rtl8139,netdev=net0" \
    --timeout 600 --steptmo 120 --ok "STOPPEDUP" --log "$LOG17" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$NEWPASS" \
    "expect=# " "send=grep -c ^extra: /etc/passwd | sed s/^/EXTRA=/; stat -c %u:%g:%a /home/extra" \
    "expect=EXTRA=1" "expect=1001:1001:700" \
    "send=test -z \"\$(ls -A /home/extra)\" && echo HOME\"\"MADE" "expect=HOMEMADE" \
    "send=test ! -e /home/worker && ! grep -rqs orphan-uid-2999 /root/.ssh /home/*/.ssh && echo ORPHAN\"\"GONE" "expect=ORPHANGONE" \
    "send=stat -c %u:%g:%a /home/work /home/work/.ssh /home/work/.ssh/authorized_keys" \
    "expect=2001:2001:700" "expect=2001:2001:700" "expect=2001:2001:600" \
    "send=stat -c %u:%g:%a /home/Alice /home/Alice/.ssh /home/Alice/.ssh/authorized_keys" \
    "expect=2002:2002:700" "expect=2002:2002:700" "expect=2002:2002:600" \
    "send=stat -c %u:%g:%a /home/alice /home/alice/.ssh /home/alice/.ssh/authorized_keys" \
    "expect=2003:2003:700" "expect=2003:2003:700" "expect=2003:2003:600" \
    "send=stat -c %u:%g:%a /srv/alice/.ssh /srv/alice/.ssh/authorized_keys" \
    "expect=2004:2004:700" "expect=2004:2004:600" \
    "send=test ! -e /srv/empty/.ssh && ! grep -q image-key /srv/alice/.ssh/authorized_keys && echo SERVICE\"\"SAFE" "expect=SERVICESAFE" \
    "send=stat -c %u:%g:%a /etc/dropbear /root/.ssh /home/test/.ssh" \
    "expect=0:0:700" "expect=0:0:700" "expect=1000:1000:700" \
    "send=stat -c %u:%g:%a /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys /home/test/.ssh/authorized_keys" \
    "expect=0:0:600" "expect=0:0:600" "expect=0:0:600" "expect=1000:1000:600" \
    "send=sha256sum /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys /home/test/.ssh/authorized_keys | sed s/^/KEYHASH:/" \
    "send=sha256sum /home/work/.ssh/authorized_keys /home/Alice/.ssh/authorized_keys /home/alice/.ssh/authorized_keys /srv/alice/.ssh/authorized_keys | sed s/^/KEYHASH:/" \
    "send=mkdir -p /r && mount -t vfat /dev/disk0p2 /r && test ! -e /r/rach2-config-a/update-in-progress && echo MARK\"\"GONE; umount /r" \
    "expect=MARKGONE" \
    "send=echo RESUME\"\"READY" "expect=RESUMEREADY" "expect=STOPPEDUP" > "$LOG17.harness" 2>&1 &
QPID=$!
trap 'kill -TERM "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
if "$RUNUTIL" until "$LOG17" RESUMEREADY 180; then
    if OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$DEMO_KEY" -p "$PORT" root@localhost 'echo RESUMEKEYLOGIN' 2>&1); then
        case "$OUT" in
            *RESUMEKEYLOGIN*) note "row 17: root logs in with its restored key: OK" ;;
            *) bad "row 17: the key login returned no result: $OUT" ;;
        esac
    else
        bad "row 17: restored key login: $OUT"
    fi
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -i "$DEMO_KEY" -p "$PORT" root@localhost 'echo STOPPEDUP > /dev/console' ||
        kill -TERM "$QPID" 2>/dev/null || true
    wait "$QPID" || bad "row 17: the resumed machine did not finish its key checks"
else
    kill -TERM "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    bad "row 17: the resumed machine did not restore the keys and their permissions"
fi
trap - EXIT INT TERM HUP
grep -a '^KEYHASH:[0-9a-f]' "$LOG17" | tr -d '\r' > "$DIR/keys.after"
if cmp -s "$DIR/keys.before" "$DIR/keys.after"; then
    note "row 17: host and account keys match the installed machine: OK"
else
    bad "row 17: host or account keys changed during the resumed update"
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
# The root's entry in the image's table, the third: its first sector from
# the MBR's bytes.
ROOT_START=$(od -An -tu4 -j $((446 + 2 * 16 + 8)) -N 4 "$DISK" | tr -d ' ')
[ -n "$ROOT_START" ] && [ "$ROOT_START" -gt 0 ] || { bad "the image's third entry has no start"; exit 1; }
dd if="$DISK" of="$R/head.vol" bs=512 skip="$ROOT_START" count=$((128 * 2048)) 2>/dev/null \
    || { bad "the root's head could not be read from the image"; exit 1; }
"$GROWSET" --lba "$ROOT_START" "$R/head.vol" $((LENOVO - ROOT_START)) > "$R/save.set" \
    || { bad "growset did not compute the save set"; exit 1; }
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
        printf ' "expect=recovery image" "expect=# "'
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
    echo 'expect=key for the recovery system'; echo 'send='
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
