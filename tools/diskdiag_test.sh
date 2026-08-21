#!/usr/bin/env sh

#
# diskdiag_test.sh: read a disk without mounting it, and without writing a byte.
#
# This is the instrument the phase E carve decision rests on. The laptop's SSD
# has a Windows installation, so the machine must be able to report what the
# drive is, how big it is and what its partition table says, and to hand out
# any sector for hashing, while leaving the platter exactly as it found it.
#
# The proof is in three boots of one freshly built disk image.
#
# The first is the control: the ordinary kernel mounting the disk, which MUST
# change the image, because the mount stamps a dirty flag. Without that step
# the two comparisons below would pass on a test that could not detect a write
# at all.
#
# The second is the diagnostic on the read-only kernel, reached over SSH with
# nothing typed at the console, which is how the laptop will be used. It checks
# what /proc/disk says, and then hashes sectors through /dev/disk0 and compares
# each against the host's hash of the same offset in the image file. That last
# comparison is the one that matters: it shows the machine handed back the
# sector the caller named, not merely some 512 bytes.
#
# The third is absence: the same kernel with no drive at all.
#
# Usage: diskdiag_test.sh <read-write kernel> <read-only kernel> <metal image>
# [port]
#

set -e

RW_KERNEL="$1"
RO_KERNEL="$2"
METAL_IMAGE="$3"
PORT="${4:-2224}"

if [ -z "$RW_KERNEL" ] || [ -z "$RO_KERNEL" ] || [ -z "$METAL_IMAGE" ]; then
    echo "usage: diskdiag_test.sh <read-write kernel> <read-only kernel> <metal image> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KEY="$REPO/krffs/rach2_demo_ed25519"
W="$REPO/build/images/diskdiag"
BASE="$W.base.disk"
RW="$W.rw.disk"
RO="$W.ro.disk"
IMG="$W.metal.krffs"
RWLOG="$W.rw.log"
ROLOG="$W.ro.log"
NOLOG="$W.none.log"
KNOWN="$W.known"

QPID=""

cleanup_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

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

wait_token() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$3" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

if [ ! -f "$KEY" ]; then
    echo "diskdiag-test: no demo key at $KEY; run 'sh krffs/gen_demo_key.sh'" >&2
    exit 2
fi

# The artifacts first: a kernel that is not the policy it claims would make
# every comparison below meaningless.
python3 "$REPO/tools/check_diskpolicy.py" "$RW_KERNEL" read-write \
    || fail "the read-write kernel is not read-write"
python3 "$REPO/tools/check_diskpolicy.py" "$RO_KERNEL" read-only \
    || fail "the read-only kernel is not read-only"

rm -f "$BASE"
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$BASE" 64 48 >/dev/null 2>&1 \
    || fail "could not build the disk image"
cp "$METAL_IMAGE" "$IMG"

# ---- 1. the control: mounting writes ----
cp "$BASE" "$RW"
python3 "$REPO/tools/harness.py" --kernel "$RW_KERNEL" \
    --drive "file=$RW,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.fsdisk rach2.rescue" \
    --timeout 90 --log "$RWLOG" --ok "Type 'help'" \
    "expect=Type 'help'" wait=3 >/dev/null 2>&1 || true
if ! grep -q "OK (KRFFS" "$RWLOG"; then
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
qemu-system-i386 -cpu max -display none -kernel "$RO_KERNEL" \
    -initrd "$IMG" \
    -append "rach2.ip=$GUEST_IP/24 rach2.gateway=$GUEST_GW rach2.disk=ahci rach2.diskdiag" \
    -drive "file=$RO,format=raw,if=none,id=drv0" \
    -device ich9-ahci,id=ahci0 -device ide-hd,drive=drv0,bus=ahci0.0 \
    -netdev "user,id=n0,net=192.168.90.0/24,host=$GUEST_GW,hostfwd=tcp::$PORT-$GUEST_IP:22" \
    -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$ROLOG" 2>&1 &
QPID=$!

wait_token "rach2 login" 90 "$ROLOG" || fail "the diagnostic boot never reached its login prompt"

ssh_run() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        root@127.0.0.1 "$1" 2>>"$ROLOG"
}

i=0
while [ "$i" -lt 60 ]; do
    if ssh_run 'echo READY' 2>/dev/null | grep -q READY; then
        break
    fi
    sleep 2
    i=$((i + 2))
done
[ "$i" -lt 60 ] || fail "the diagnostic boot never answered SSH"

DISKTXT="$(ssh_run 'cat /proc/disk' | tr -d '\r')"
for want in "state present" "driver ahci" "policy read-only" "mounted 0" "mbr-signature ok"; do
    echo "$DISKTXT" | grep -q "^$want\$" || fail "/proc/disk does not say '$want'"
done
# The sector count the machine reports must be the image's real size, computed
# here, not written down, so a wrong IDENTIFY cannot agree with a
# hardcoded number.
IMG_SECTORS=$(( $(wc -c < "$BASE") / 512 ))
echo "$DISKTXT" | grep -q "^sectors $IMG_SECTORS\$" \
    || fail "/proc/disk reports a sector count that is not the image's $IMG_SECTORS"
echo "$DISKTXT" | grep -q "^model " || fail "/proc/disk has no model"
echo "diskdiag-test: /proc/disk reports the drive, its geometry and its table: OK"

# The sectors themselves. Each is hashed on the machine through /dev/disk0 and
# on the host straight out of the image file; the two must agree, which is what
# shows the machine returned the sector that was named.
check_sector() {
    lba="$1"
    guest="$(ssh_run "dd if=/dev/disk0 bs=512 skip=$lba count=1 2>/dev/null | sha256sum" \
             | tr -d '\r' | awk '{print $1}')"
    host="$(dd if="$BASE" bs=512 skip="$lba" count=1 2>/dev/null | shasum -a 256 | awk '{print $1}')"
    if [ -z "$guest" ] || [ "$guest" != "$host" ]; then
        fail "sector $lba read back as $guest and the image has $host"
    fi
}
check_sector 0
check_sector 2048
check_sector $((IMG_SECTORS - 1))
echo "diskdiag-test: sectors 0, 2048 and $((IMG_SECTORS - 1)) hash the same on the machine and here: OK"

# A write to the window is refused, and the node is root-only.
ssh_run 'dd if=/dev/zero of=/dev/disk0 bs=512 count=1 2>&1 | head -1' >/dev/null 2>&1 || true
if grep -q "refused command" "$ROLOG"; then
    fail "the diagnostic path issued a command the allowlist refused; it strayed off the read path"
fi
cleanup_boot
if ! cmp -s "$BASE" "$RO"; then
    fail "the diagnostic boot changed the disk image"
fi
echo "diskdiag-test: the whole session left the disk byte for byte: OK"

# ---- 3. absence ----
python3 "$REPO/tools/harness.py" --kernel "$RO_KERNEL" \
    --initrd "$IMG" --qemu-arg=-cpu --qemu-arg=max \
    --append "rach2.diskdiag rach2.rescue" \
    --timeout 90 --log "$NOLOG" --ok "Disk diagnostic" \
    "expect=Type 'help'" wait=3 >/dev/null 2>&1 || true
grep -q "Disk diagnostic... none" "$NOLOG" \
    || fail "with no drive the diagnostic did not report an absent disk"
echo "diskdiag-test: with no drive at all it says so instead of pretending: OK"

echo "diskdiag-test: PASS"
