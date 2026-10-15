#!/usr/bin/env sh

#
# diskdiag.sh: a disk read without mounting it, and without a byte written.
#
# The read-only kernel's diagnostic reports what a drive is, how big it is and
# what its partition table says, and hands out any sector for hashing, so that
# a disk whose data has no backup can be examined without being changed. The
# suite boots one freshly built disk image three times.
#
# The first boot is the control: the ordinary kernel mounts the disk, which
# must change the image, since the mount stamps a dirty flag. Without it the
# comparisons below would pass on a test that could not detect a write.
#
# The second runs the diagnostic on the read-only kernel, reached over SSH
# with nothing typed at the console. It checks what /proc/disk says, then
# hashes sectors through /dev/disk0 and compares each with the host's hash of
# the same offset in the image file, which shows that the machine returned
# the sector named and not some other 512 bytes. A write to /dev/disk0 must
# not reach the drive, and the image must be unchanged after the boot.
#
# The third boots the same kernel with no drive at all, and the diagnostic
# must report the disk absent.
#
# Usage: diskdiag.sh <read-write kernel> <read-only kernel> <metal image>
# [port]
#

# The build tree and the emulator of the architecture
# tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}
QEMU=${RACH2_QEMU:-qemu-system-i386}

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

RW_KERNEL="$1"
RO_KERNEL="$2"
METAL_IMAGE="$3"
PORT="${4:-2225}"

if [ -z "$RW_KERNEL" ] || [ -z "$RO_KERNEL" ] || [ -z "$METAL_IMAGE" ]; then
    echo "usage: diskdiag_test.sh <read-write kernel> <read-only kernel> <metal image> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
KEY="$REPO/krffs/rach2_demo_ed25519"
W="$REPO/$ARCH_BUILD/images/diskdiag"
BASE="$W.base.disk"
RW="$W.rw.disk"
RO="$W.ro.disk"
IMG="$W.metal.krffs"
RWLOG="$W.rw.log"
ROLOG="$W.ro.log"
NOLOG="$W.none.log"
KNOWN="$W.known"

QPID=""

# Stop the machine, if it runs.
cleanup_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# Stop the machine and remove the disks, the image and the logs.
cleanup() {
    cleanup_boot
    rm -f "$BASE" "$RW" "$RO" "$IMG" "$RWLOG" "$ROLOG" "$NOLOG" "$KNOWN"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    cleanup_boot
    echo "diskdiag-test: FAIL: $1" >&2
    exit 1
}

# wait_token TOKEN SECONDS LOG: wait up to SECONDS for TOKEN in LOG.
wait_token() {
    "$RUNUTIL" until "$3" "$1" "$2"
}

if [ ! -f "$KEY" ]; then
    echo "diskdiag-test: no demo key at $KEY; run 'sh krffs/gen_demo_key.sh'" >&2
    exit 2
fi

# The artifacts first: the comparisons below mean nothing for a kernel built
# with the other policy.
sh "$REPO/tools/inspect/check_diskpolicy.sh" "$RW_KERNEL" read-write \
    || fail "the read-write kernel is not read-write"
sh "$REPO/tools/inspect/check_diskpolicy.sh" "$RO_KERNEL" read-only \
    || fail "the read-only kernel is not read-only"

rm -f "$BASE"
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$BASE" 64 48 >/dev/null 2>&1 \
    || fail "could not build the disk image"
cp "$METAL_IMAGE" "$IMG"

# ---- 1. the control: mounting writes ----
cp "$BASE" "$RW"
"$REPO/build/tests/system/harness" --kernel "$RW_KERNEL" \
    --drive "file=$RW,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.fsdisk rach2.rescue" \
    --timeout 90 --log "$RWLOG" --ok "Type 'help'" \
    "expect=krffs: mounted:" "expect=Type 'help'" >/dev/null 2>&1 || true
if ! grep -q "krffs: mounted:" "$RWLOG"; then
    fail "the control boot did not mount the disk, so the comparisons prove nothing"
fi
if cmp -s "$BASE" "$RW"; then
    fail "the control boot left the disk unchanged, so this test cannot detect a write"
fi
echo "diskdiag-test: the control boot mounted the disk and wrote to it: OK"

# ---- 2. the diagnostic, over SSH, on the read-only kernel ----
cp "$BASE" "$RO"
GUEST_IP="192.168.90.15"
GUEST_GW="192.168.90.2"
"$QEMU" -cpu max -display none -kernel "$RO_KERNEL" \
    -initrd "$IMG" \
    -append "rach2.ip=$GUEST_IP/24 rach2.gateway=$GUEST_GW rach2.disk=ahci rach2.diskdiag" \
    -drive "file=$RO,format=raw,if=none,id=drv0" \
    -device ich9-ahci,id=ahci0 -device ide-hd,drive=drv0,bus=ahci0.0 \
    -netdev "user,id=n0,net=192.168.90.0/24,host=$GUEST_GW,hostfwd=tcp::$PORT-$GUEST_IP:22" \
    -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$ROLOG" 2>&1 &
QPID=$!

wait_token "rach2 login" 90 "$ROLOG" || fail "the diagnostic boot never reached its login prompt"

# ssh_run COMMAND: run COMMAND as root on the machine over SSH with the demo
# key, its errors appended to the console's log.
ssh_run() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes \
        root@127.0.0.1 "$1" 2>>"$ROLOG"
}

i=0
while [ "$i" -lt "$(scaled 60)" ]; do
    if ssh_run 'echo READY' 2>/dev/null | grep -q READY; then
        break
    fi
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
    i=$((i + 2))
done
[ "$i" -lt "$(scaled 60)" ] || fail "the diagnostic boot never answered SSH"

DISKTXT="$(ssh_run 'cat /proc/disk' | tr -d '\r')"
for want in "state present" "driver ahci" "policy read-only" "mounted 0" "mbr-signature ok"; do
    echo "$DISKTXT" | grep -q "^$want\$" || fail "/proc/disk does not say '$want'"
done
# The sector count the machine reports must be the image's size, computed
# here, so that a wrong IDENTIFY cannot agree with a number written into the
# test.
IMG_SECTORS=$(( $(wc -c < "$BASE") / 512 ))
echo "$DISKTXT" | grep -q "^sectors $IMG_SECTORS\$" \
    || fail "/proc/disk reports a sector count that is not the image's $IMG_SECTORS"
echo "$DISKTXT" | grep -q "^model " || fail "/proc/disk has no model"
echo "diskdiag-test: /proc/disk reports the drive, its geometry and its table: OK"

# check_sector LBA: hash sector LBA on the machine through /dev/disk0 and on
# the host out of the image file, and fail unless the two agree, which shows
# that the machine returned the sector that was named.
check_sector() {
    lba="$1"
    guest="$(ssh_run "dd if=/dev/disk0 bs=512 skip=$lba count=1 2>/dev/null | sha256sum" \
             | tr -d '\r' | awk '{print $1}')"
    host="$(dd if="$BASE" bs=512 skip="$lba" count=1 2>/dev/null \
           | "$REPO/build/tools/verify/sha256" - | awk '{print $1}')"
    if [ -z "$guest" ] || [ "$guest" != "$host" ]; then
        fail "sector $lba read back as $guest and the image has $host"
    fi
}
check_sector 0
check_sector 2048
check_sector $((IMG_SECTORS - 1))
echo "diskdiag-test: sectors 0, 2048 and $((IMG_SECTORS - 1)) hash the same on the machine and here: OK"

# A write to /dev/disk0 is refused above the driver: no command reaches the
# drive's allow list to be refused there.
ssh_run 'dd if=/dev/zero of=/dev/disk0 bs=512 count=1 2>&1 | head -1' >/dev/null 2>&1 || true
if grep -q "refused: command" "$ROLOG"; then
    fail "the diagnostic path issued a command the allowlist refused; it strayed off the read path"
fi
cleanup_boot
if ! cmp -s "$BASE" "$RO"; then
    fail "the diagnostic boot changed the disk image"
fi
echo "diskdiag-test: the whole session left the disk byte for byte: OK"

# ---- 3. absence ----
"$REPO/build/tests/system/harness" --kernel "$RO_KERNEL" \
    --initrd "$IMG" --qemu-arg=-cpu --qemu-arg=max \
    --append "rach2.diskdiag rach2.rescue" \
    --timeout 90 --log "$NOLOG" --ok "diskdiag: " \
    "expect=diskdiag: absent" "expect=Type 'help'" >/dev/null 2>&1 || true
grep -q "diskdiag: absent: no disk answered" "$NOLOG" \
    || fail "with no drive the diagnostic did not report an absent disk"
echo "diskdiag-test: with no drive at all it says so instead of pretending: OK"

echo "diskdiag-test: PASS"
