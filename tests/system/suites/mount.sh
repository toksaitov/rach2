#!/usr/bin/env sh
#
# mount.sh: the block path under the mount milestone, phase 2's boots.
#
# The disk is the KRFFS test disk with a FAT32 partition added after it, so
# the partition nodes have two real entries to publish. The lag boots prove
# the sleeping path: a reader that sleeps while another process runs, and a
# device that never answers in time being quarantined and freed late.
#
# Usage: mount.sh KERNEL IMAGE HARNESS DISK OSREAL PASSWORD
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; OSREAL=$5; PASSWORD=$6
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "mount-test: FAIL: $1" >&2; exit 1; }

# The two-partition disk: 48 MiB KRFFS at LBA 2048, 14 MiB FAT32 at LBA 100352.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DISK" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:98304:7f 100352:28672:0c >/dev/null || fail "mkmbr"
# -T: the volume is the partition's 28672 sectors, not the rest of the
# image; the driver refuses a volume that claims more sectors than the
# partition has.
mformat -i "$DISK"@@51380224 -F -T 28672 -v M36FAT :: || fail "mformat"
echo "hello from the mount suite" > "${DISK}.hello"
mcopy -i "$DISK"@@51380224 "${DISK}.hello" ::/hello.txt || fail "mcopy"
# A program on the volume, for the noexec and the shared-description rows,
# and a name written and deleted and written again so the volume is
# fragmented the way a used one is.
mcopy -i "$DISK"@@51380224 "$REPO/build/tests/hello" ::/hello || fail "mcopy hello"
mcopy -i "$DISK"@@51380224 "${DISK}.hello" ::/gone.txt || fail "mcopy gone"
mdel -i "$DISK"@@51380224 ::/gone.txt || fail "mdel"
mcopy -i "$DISK"@@51380224 "$REPO/build/tests/hello" ::/again.bin || fail "mcopy again"
# 128 sectors, each 128 copies of its own number and a dot, so a reader can
# tell which sector it was given.
awk 'BEGIN { for (i = 0; i < 128; i++) for (j = 0; j < 128; j++) printf "%03d.", i }' \
    > "${DISK}.pattern" || fail "pattern"
mcopy -i "$DISK"@@51380224 "${DISK}.pattern" ::/pattern.bin || fail "mcopy pattern"

run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# ---- 1: the partition nodes appear with the sizes the MBR gives, and a read
#         through one, on the lag, sleeps the reader while its sibling runs ----
if run --append "rach2.blocklag=10 rach2.rescue" \
    --timeout 90 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe sleep" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: the lag reader sleeps while its sibling counts: OK"
else
    fail "the lag sleep boot"
fi

# ---- 2: a device that never answers in time: the deadline gives EIO, the
#         late completion frees the quarantined slot, and the pool serves the
#         next request ----
if run --append "rach2.blocklag=700 rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe deadline" \
    "expect=quarantined" "expect=late completion" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: the deadline quarantines and the late answer frees: OK"
else
    fail "the deadline boot"
fi

# ---- 3: the root mounts through the real device while the partitions are on
#         the lag, and /proc/mounts names the partition as the root ----
if run --append "rach2.blocklag=3 rach2.fsdisk rach2.rescue" \
    --timeout 90 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe root" \
    "expect=LAGPROBE" > /dev/null 2>&1; then
    echo "mount-test: the root is on the real device beside the lag: OK"
else
    fail "the root-beside-lag boot"
fi

# ---- 4: a write through the lag: the kernel copies the caller's bytes into
#         the request page before the device sleeps, and the sector reads back
#         through the same sleeping path. /dev/bootpart is the one writable
#         raw window, so the disk is the boot-fixture one ----
BOOTDISK="${DISK%.disk}-boot.disk"
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$BOOTDISK" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk (boot fixture)"
"$REPO/build/tests/system/mkfixture" boot "$BOOTDISK" >/dev/null || fail "mkfixture boot"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$BOOTDISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.blocklag=10 rach2.rescue" \
    --timeout 90 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe write" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: a sector written through the lag reads back: OK"
else
    fail "the bootpart write boot"
fi

# ---- 5: mount(2) and umount2(2), the lifetime of a mount, the refused
#         flags, the claims, and the /proc files, from a program ----
if run --append "rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "MOUNTPROBE OK" --never "MOUNTPROBE FAIL" \
    --log "$DISK.mountprobe.log" \
    "expect=Type 'help'" "send=exec /mountprobe" \
    "expect=MOUNTPROBE" > /dev/null 2>&1; then
    echo "mount-test: mount, umount2 and the lifetime of a mount: OK"
else
    fail "the mountprobe boot"
fi
# The unmount record: every open matched by a release, on every unmount.
if grep -a 'unmounted /mnt' "$DISK.mountprobe.log" | \
    awk 'BEGIN { bad = 1 } { bad = ($5 != $7 || $5 !~ /^[0-9]+$/) ? 1 : 0 } END { exit bad }'; then
    echo "mount-test: every unmount record has opens equal to releases: OK"
else
    fail "an unmount record's opens and releases differ"
fi

# ---- 6: the mounted half of the lag probe; diskdiag publishes /dev/disk0 on
#         the real disk while the partitions are on the lag ----
if run --append "rach2.blocklag=3 rach2.diskdiag rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe mounted" \
    "expect=LAGPROBE" > /dev/null 2>&1; then
    echo "mount-test: the mounted half sleeps, waits and shares as it should: OK"
else
    fail "the lag mounted boot"
fi

# ---- 7: the boot partition mounted read-only refuses the raw writer, which
#         is the bootctl conflict answered with EBUSY. The disk is the suite's
#         with its MBR entries swapped, so entry 0, the boot partition, is
#         the FAT volume: /dev/bootpart and /dev/disk0p1 then name one range
#         under two names, which is what the claims compare ----
CLAIMDISK="${DISK%.disk}-claim.disk"
cp "$DISK" "$CLAIMDISK" || fail "copy for the claim disk"
"$REPO/build/tools/image/mkmbr" --lba "$CLAIMDISK" 100352:28672:0c 2048:98304:7f >/dev/null \
    || fail "mkmbr (claim)"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$CLAIMDISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue" \
    --timeout 90 --ok "CLAIMPROBE OK" --never "CLAIMPROBE FAIL" \
    "expect=Type 'help'" "send=exec /mountprobe claim" \
    "expect=CLAIMPROBE" > /dev/null 2>&1; then
    echo "mount-test: a mounted boot partition refuses the raw writer: OK"
else
    fail "the claim boot"
fi

# ---- 8: BusyBox on os-real mounts, lists, reads and unmounts through the
#         shell; the listing is mdir's and the bytes are mcopy's ----
LOG="$DISK.busybox.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --timeout 240 --steptmo 60 --ok "BBDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir /mnt && mount -t vfat -o ro /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=ls /mnt | sort | tr '\\n' ' '; echo; echo LIST\"\"DONE" \
    "expect=LISTDONE" "send=cat /mnt/hello.txt; cksum /mnt/hello /mnt/again.bin /mnt/hello.txt; echo SUMS\"\"DONE" \
    "expect=SUMSDONE" "send=sleep 5 < /mnt/hello.txt & umount /mnt; echo UMOUNT\"\"RC=\$?" \
    "expect=UMOUNTRC=1" wait=6 "send=umount /mnt && echo UN\"\"MOUNTED" \
    "expect=UNMOUNTED" "send=[ \"\$(grep -c vfat /proc/mounts)\" = 0 ] && echo BB\"\"DONE" \
    "expect=BBDONE" > /dev/null 2>&1; then
    echo "mount-test: BusyBox mounts, lists, reads and unmounts: OK"
else
    fail "the BusyBox boot"
fi
# The listing the shell printed against the one mtools reads off the image.
HOST=$(mdir -i "$DISK"@@51380224 -b :: | sed 's#::/##' | sort | tr '\n' ' ' | sed 's/ *$//')
GUEST=$(grep -a '^again.bin' "$LOG" | head -1 | tr -d '\r' | sed 's/ *$//')
if [ "$GUEST" = "$HOST" ]; then
    echo "mount-test: the shell's listing is mdir's: OK"
else
    fail "the listing differs: guest '$GUEST' host '$HOST'"
fi
if grep -aq 'Resource busy' "$LOG"; then
    echo "mount-test: umount answered EBUSY with a file open: OK"
else
    fail "umount did not answer EBUSY with a file open"
fi
# Every file's bytes: the checksums the guest printed against the ones the
# host computes over what mcopy reads off the image.
for name in hello again.bin hello.txt; do
    want=$(mcopy -i "$DISK"@@51380224 "::/$name" - | cksum | awk '{print $1, $2}')
    got=$(grep -a "/mnt/$name" "$LOG" | grep -av cksum | head -1 | tr -d '\r' | awk '{print $1, $2}')
    if [ "$want" != "$got" ]; then
        fail "the bytes of $name differ: guest '$got' host '$want'"
    fi
done
echo "mount-test: every file's bytes are what mcopy reads: OK"

# ---- 9: a write to the volume that the device fails: the mount goes
#         read-only with a record, and stays so ----
if run --append "rach2.blocklag=3 rach2.blockfail=3 rach2.rescue" \
    --timeout 90 --steptmo 60 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    --log "$DISK.blockfail.log" \
    "expect=Type 'help'" "send=exec /lagprobe blockfail" \
    "expect=LAGPROBE" > /dev/null 2>&1 && grep -aq 'vfs: read-only: /lb' "$DISK.blockfail.log"; then
    echo "mount-test: a failed write puts the mount read-only with a record: OK"
else
    fail "the blockfail boot"
fi

echo "mount-test: PASS"
