#!/bin/sh
# krffsmount.sh: a KRFFS volume mounted by mount(2), read, written, read back
# after a reboot, taken through a machine that was killed while writing, and
# written by two processes at once; and the volumes the kernel must refuse.
# The host's fsck.krffs judges the volume after every boot that wrote it.
#
# Usage: krffsmount.sh KERNEL IMAGE HARNESS DISK FSCK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; FSCK=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
DIR="$REPO/build/tests/system/krffsvol"
VOL="$DIR/volume.krffs"
PART=2048

# fail REASON: print REASON and exit 1.
fail() { echo "krffsmount-test: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR"
python3 - "$DIR/pattern.bin.$$" <<'PY' || fail "the pattern file"
import sys
open(sys.argv[1], 'wb').write(bytes((i * 31 + 7) & 0xFF for i in range(65536)))
PY
printf 'hello\n' > "$DIR/hello.txt.$$" || fail "the hello file"
printf 'inner\n' > "$DIR/inner.txt.$$" || fail "the inner file"
# Into place by rename: `tests/run krffscheck` writes the same three files
# for the same manifest, and the two may run at once.
for f in pattern.bin hello.txt inner.txt; do
    mv "$DIR/$f.$$" "$DIR/$f" || fail "the fixture files"
done

sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/krffsvol-test.manifest" \
    "$VOL" 4 >/dev/null 2>&1 || fail "building the volume"
SECTORS=$(( $(wc -c < "$VOL") / 512 ))

# build_disk VOLUME [PAD]: write the disk as a megabyte of zeros, VOLUME and
# PAD sectors of zeros, with a partition of SECTORS sectors at the volume.
build_disk() {
    { dd if=/dev/zero bs=512 count=$PART 2>/dev/null; cat "$1"; \
      dd if=/dev/zero bs=512 count="${2:-0}" 2>/dev/null; } > "$DISK"
    "$REPO/build/tools/image/mkmbr" --lba "$DISK" "$PART:$SECTORS:7f" >/dev/null \
        || fail "the partition table"
}

# build_three: write the disk with three copies of the volume, each in a
# partition of its own, for the row that mounts them all at once.
build_three() {
    three=$(($(wc -c < "$VOL") / 512))
    { dd if=/dev/zero bs=512 count=$PART 2>/dev/null; cat "$VOL"; cat "$VOL"; \
      cat "$VOL"; } > "$DISK"
    "$REPO/build/tools/image/mkmbr" --lba "$DISK" \
        "$PART:$three:7f" \
        "$((PART + three)):$three:7f" \
        "$((PART + 2 * three)):$three:7f" >/dev/null \
        || fail "the partition table of three"
}

# run ARG...: boot the test image on the disk with the harness's ARGs.
run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# ---- 1: mounted, read, unmounted ----
build_disk "$VOL"
if run --append "rach2.rescue" --timeout 90 --steptmo 60 \
    --ok "KRFFSPROBE OK (read)" --never "KRFFSPROBE FAIL" --log "$DISK.read.log" \
    "expect=Type 'help'" "send=exec /krffsprobe read" "expect=KRFFSPROBE" \
    > /dev/null 2>&1; then
    echo "krffsmount-test: a volume is mounted, read to the last byte, and unmounted: OK"
else
    fail "the read boot"
fi
grep -aq 'krffs: mount: .* read-only' "$DISK.read.log" \
    || fail "the mount did not say what it placed"

# The image the boot read is byte for byte the image the host built: a
# read-only mount writes nothing, and a cache that wrote anything would show
# here.
dd if="$DISK" of="$DISK.after" bs=512 skip=$PART count=$SECTORS 2>/dev/null
cmp -s "$VOL" "$DISK.after" \
    || fail "the read-only mount changed the volume"
echo "krffsmount-test: and the volume is byte for byte what it was: OK"

# take_volume: copy the volume out of the disk into $DISK.vol, for fsck.krffs,
# the host's check of everything the kernel writes.
take_volume() {
    dd if="$DISK" of="$DISK.vol" bs=512 skip=$PART count=$SECTORS 2>/dev/null
}
# strict_clean REASON: fail with REASON unless `fsck.krffs --strict`, which
# refuses an image that needs any repair, passes the volume.
strict_clean() {
    take_volume
    "$FSCK" "$DISK.vol" --strict > "$DISK.fsck" 2>&1 \
        || { cat "$DISK.fsck" >&2; fail "$1"; }
}

# ---- 2: written from a program, and the host finds it consistent ----
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (write)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.write.log" "expect=Type 'help'" \
    "send=exec /krffsprobe write" "expect=KRFFSPROBE" > /dev/null 2>&1; then
    echo "krffsmount-test: a program writes the volume and unmounts it: OK"
else
    fail "the write boot"
fi
grep -aq 'krffs: mount: .* read-write' "$DISK.write.log" \
    || fail "the write boot did not mount for writing"
strict_clean "the volume needs repair after a clean unmount"
echo "krffsmount-test: the host's checker finds it consistent and clean: OK"

# ---- 3: the next boot finds everything where it was left ----
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (readback)" --never "KRFFSPROBE FAIL" \
    "expect=Type 'help'" "send=exec /krffsprobe readback" "expect=KRFFSPROBE" \
    > /dev/null 2>&1; then
    echo "krffsmount-test: every byte, name and mode is there after a reboot: OK"
else
    fail "the read-back boot"
fi

# ---- 4: a machine killed while writing, and the mount that repairs it ----
run --append "rach2.rescue" --timeout 60 --steptmo 30 --ok "KRFFSPROBE WRITING" \
    --never "KRFFSPROBE FAIL" "expect=Type 'help'" \
    "send=exec /krffsprobe crash" "expect=KRFFSPROBE WRITING" > /dev/null 2>&1 \
    || fail "the crash boot"
take_volume
"$FSCK" "$DISK.vol" --strict > "$DISK.fsck" 2>&1 \
    && fail "the killed machine left the volume calling itself clean"
grep -q 'not cleanly unmounted' "$DISK.fsck" \
    || { cat "$DISK.fsck" >&2; fail "the checker did not see the dirty flag"; }
echo "krffsmount-test: a machine killed while writing leaves the dirty flag set: OK"

if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (readback)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.repair.log" "expect=Type 'help'" \
    "send=exec /krffsprobe readback" "expect=KRFFSPROBE" > /dev/null 2>&1; then
    echo "krffsmount-test: the volume still reads back after the machine was killed: OK"
else
    fail "the boot after the crash"
fi
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (write)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.repair2.log" "expect=Type 'help'" \
    "send=exec /krffsprobe write" "expect=KRFFSPROBE" > /dev/null 2>&1 \
    && grep -aq 'krffs: check: the volume was not unmounted cleanly' "$DISK.repair2.log"; then
    # What the check repairs is the row below, on a volume broken on purpose:
    # a killed machine need not have left anything to repair, and a row that
    # accepts a count of none would say nothing either way.
    echo "krffsmount-test: the next mount that writes checks the volume it was left: OK"
else
    fail "the repair boot"
fi
strict_clean "the volume needs repair after the mount that repaired it"

# ---- 5: what the mount repairs, on a volume broken on purpose ----
# The three faults the format's order of writes can leave: a backward link the
# forward chain contradicts, a reserved node with no name that no path
# reaches, and one name on two nodes of a directory. The dirty flag is what
# tells the mount a check is owed.
python3 - "$VOL" "$DIR/broken.krffs" <<'PY' || fail "the volume to repair"
import struct, sys

HDR = 357
TYPE, NAME, MODE, SIZE, PREV, DATA, PARENT = 2, 11, 267, 325, 333, 341, 349
b = bytearray(open(sys.argv[1], 'rb').read())

# A check is owed.
struct.pack_into('<H', b, MODE, struct.unpack_from('<H', b, MODE)[0] | 0o4000)

nodes, off = [], 0
while off < len(b):
    size = struct.unpack_from('<Q', b, off + SIZE)[0]
    nodes.append((off, size, b[off + TYPE], bytes(b[off + NAME:off + NAME + 32])))
    off += size

reserved = [n for n in nodes if n[2] == 2 and n[3][0] != 0]
if len(reserved) < 3:
    raise SystemExit('the volume has too few files for these rows')

# A backward link that disagrees with the forward chain.
struct.pack_into('<Q', b, nodes[1][0] + PREV, 12345)
# A node no path reaches: a reserved node whose name is gone.
b[reserved[0][0] + NAME] = 0
# One name on two nodes: the second takes the third's name and directory.
b[reserved[1][0] + NAME:reserved[1][0] + NAME + 256] = \
    b[reserved[2][0] + NAME:reserved[2][0] + NAME + 256]
struct.pack_into('<Q', b, reserved[1][0] + PARENT,
                 struct.unpack_from('<Q', b, reserved[2][0] + PARENT)[0])
open(sys.argv[2], 'wb').write(b)
PY
build_disk "$DIR/broken.krffs"
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (write)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.broken.log" "expect=Type 'help'" \
    "send=exec /krffsprobe write" "expect=KRFFSPROBE" > /dev/null 2>&1 \
    && grep -aq 'krffs: check: repaired: 1 backward link(s), 2 orphan(s), 1 repeated name(s)' \
        "$DISK.broken.log"; then
    # Two nameless nodes, not one: the loser of the repeated name gives up its
    # name and is reclaimed in the same pass as the node that had none.
    echo "krffsmount-test: a mount repairs a wrong backward link, a nameless node and a repeated name, and says how many: OK"
else
    grep -a 'krffs: check' "$DISK.broken.log" >&2 || true
    fail "the repair of a volume broken on purpose"
fi
strict_clean "the repaired volume still needs repair"

build_disk "$VOL"

# ---- 6: two writers on one mount ----
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (writers)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.writers.log" "expect=Type 'help'" "send=exec /krffsprobe writers" "expect=KRFFSPROBE" \
    > /dev/null 2>&1; then
    echo "krffsmount-test: two writers on one mount write files that read back: OK"
else
    fail "the two-writer boot"
fi
strict_clean "the volume needs repair after two writers"

# The rows below start from the image again, since the ones above changed it.
build_disk "$VOL"

# ---- the volumes the kernel refuses ----
# refuse WHAT LINE: boot the refusal probe on the disk and fail unless the
# kernel refused the volume and its log has LINE.
refuse() {
    what=$1; line=$2
    if run --append "rach2.rescue" --timeout 90 --steptmo 60 \
        --ok "KRFFSPROBE OK (refuse)" --never "KRFFSPROBE FAIL" \
        --log "$DISK.$what.log" "expect=Type 'help'" \
        "send=exec /krffsprobe refuse" "expect=KRFFSPROBE" > /dev/null 2>&1; then
        grep -aq "$line" "$DISK.$what.log" \
            || fail "the kernel refused the $what volume without saying why"
        echo "krffsmount-test: a volume with $what is refused, by name: OK"
    else
        fail "the $what boot"
    fi
}

python3 - "$VOL" "$DIR/badmagic.krffs" <<'PY' || fail "the bad-magic volume"
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
b[0] = 0x00
open(sys.argv[2], 'wb').write(b)
PY
build_disk "$DIR/badmagic.krffs"
refuse "a bad signature" "krffs: mount: refused: magic"

python3 - "$VOL" "$DIR/badchain.krffs" <<'PY' || fail "the malformed volume"
import struct, sys
SIZE = 325
b = bytearray(open(sys.argv[1], 'rb').read())
# The root's size, made one byte longer: every node after it is then at the
# wrong place and the ring cannot close.
struct.pack_into('<Q', b, SIZE, struct.unpack_from('<Q', b, SIZE)[0] + 1)
open(sys.argv[2], 'wb').write(b)
PY
build_disk "$DIR/badchain.krffs"
refuse "a malformed chain" "krffs: mount: refused: the node chain is malformed"

# A region the ring does not tile: the same image on a partition longer than
# the image it was made in.
SECTORS=$((SECTORS + 8))
build_disk "$VOL" 8
refuse "a region the ring does not fill" "krffs: mount: refused: the node chain is malformed"

# ---- a write the device fails, and the flag the unmount must leave ----
# The row above leaves SECTORS eight sectors longer than the volume, which is
# what that refusal tests; this row wants the volume's own length again.
SECTORS=$(($(wc -c < "$VOL") / 512))
build_disk "$VOL"
if run --append "rach2.rescue rach2.blocklag=1 rach2.blockfail=24" \
    --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (failed)" --never "KRFFSPROBE FAIL" \
    --log "$DISK.failed.log" "expect=Type 'help'" \
    "send=exec /krffsprobe failed" "expect=KRFFSPROBE" > /dev/null 2>&1; then
    echo "krffsmount-test: a write the device failed takes the mount read-only and the unmount writes nothing: OK"
else
    grep -a 'KRFFSPROBE FAIL' "$DISK.failed.log" >&2 || true
    fail "the failed-write boot"
fi
grep -aq 'read-only' "$DISK.failed.log" || fail "the mount did not go read-only"
take_volume
"$FSCK" "$DISK.vol" --strict > "$DISK.fsck" 2>&1 \
    && fail "the volume calls itself clean after a write the device failed"
grep -q 'not cleanly unmounted' "$DISK.fsck" \
    || { cat "$DISK.fsck" >&2; fail "the checker did not find the dirty flag"; }
echo "krffsmount-test: and the volume still says a check is owed: OK"

# ---- three volumes at once ----
build_three
if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
    --ok "KRFFSPROBE OK (three)" --never "KRFFSPROBE FAIL" --log "$DISK.two.log" \
    "expect=Type 'help'" "send=exec /krffsprobe three" "expect=KRFFSPROBE" \
    > /dev/null 2>&1; then
    echo "krffsmount-test: three volumes are mounted at once, each holding only its own files: OK"
else
    grep -a 'KRFFSPROBE FAIL' "$DISK.two.log" >&2 || true
    fail "the two-volume boot"
fi

# ---- a stick pulled from under a mounted volume ----
#
# The medium goes while the volume is mounted and being written: the machine
# must return an error, write nothing to a device that is not there, and let
# an unmount take the mount down. The same stick is then plugged back in
# while the old mount is still there, as a person would, which gives the
# driver a second device to find while the first is still held: a kernel that
# gave the new stick the old one's slot would hand the mount a pointer into
# another device, and would most likely panic here.
STICK="$DISK.stick"
# The partition is the volume's own length: a ring that does not fill its
# region is refused, which is the row above and not this one.
VOLSECTORS=$(($(wc -c < "$VOL") / 512))
dd if=/dev/zero of="$STICK" bs=1048576 count=32 2>/dev/null || fail "the stick image"
"$REPO/build/tools/image/mkmbr" --lba "$STICK" "2048:$VOLSECTORS:83" > /dev/null \
    || fail "the stick's partition table"
dd if="$VOL" of="$STICK" bs=512 seek=2048 conv=notrunc 2>/dev/null \
    || fail "the volume on the stick"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-drive --qemu-arg="file=$STICK,format=raw,if=none,id=kst" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=kst,id=kst0 \
    --append "rach2.rescue" --timeout 150 --steptmo 120 \
    --ok "KRFFSPROBE OK (pull)" --never "KRFFSPROBE FAIL" --log "$DISK.pull.log" \
    "expect=Type 'help'" "send=exec /krffsprobe pull" \
    "expect=KRFFSPROBE HOLDING" "usb=del:kst0" \
    "expect=KRFFSPROBE GONE" \
    "drive=add:if=none,id=kst2,format=raw,file=$STICK" \
    "usb=add:usb-storage,bus=xhci.0,drive=kst2,id=kst1" \
    "expect=KRFFSPROBE OK" \
    > /dev/null 2>&1; then
    echo "krffsmount-test: a stick pulled from under a mounted volume leaves a machine that unmounts it: OK"
else
    grep -a 'panic\|usbdisk\|krffs' "$DISK.pull.log" | tail -5 >&2 || true
    fail "the stick-pull boot"
fi
grep -aq "usbdisk: gone" "$DISK.pull.log" || fail "the kernel did not say the stick had gone"

echo "krffsmount-test: PASS"
