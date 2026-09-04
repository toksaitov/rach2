#!/bin/sh
# krffsmount.sh: a KRFFS volume mounted by mount(2), read, and unmounted, and
# the volumes the kernel must refuse. Four boots on a disk whose only
# partition is a KRFFS image the host tools built.
#
# Usage: krffsmount.sh KERNEL IMAGE HARNESS DISK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
DIR="$REPO/build/tests/system/krffsvol"
VOL="$DIR/volume.krffs"
PART=2048

fail() { echo "krffsmount-test: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR"
python3 - "$DIR/pattern.bin" <<'PY' || fail "the pattern file"
import sys
open(sys.argv[1], 'wb').write(bytes((i * 31 + 7) & 0xFF for i in range(65536)))
PY
printf 'hello\n' > "$DIR/hello.txt"
printf 'inner\n' > "$DIR/inner.txt"

sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/krffsvol-test.manifest" \
    "$VOL" 4 >/dev/null 2>&1 || fail "building the volume"
SECTORS=$(( $(wc -c < "$VOL") / 512 ))

# The disk: a megabyte of nothing, the volume, whatever padding the row asks
# for, and a partition table naming the whole of it.
build_disk() {
    { dd if=/dev/zero bs=512 count=$PART 2>/dev/null; cat "$1"; \
      dd if=/dev/zero bs=512 count="${2:-0}" 2>/dev/null; } > "$DISK"
    "$REPO/build/tools/image/mkmbr" --lba "$DISK" "$PART:$SECTORS:7f" >/dev/null \
        || fail "the partition table"
}

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

# ---- 2, 3, 4: the volumes the kernel refuses ----
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

echo "krffsmount-test: PASS"
