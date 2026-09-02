#!/usr/bin/env sh
#
# ext2.sh: the ext2 driver booted (`tests/run ext2`).
#
# The fixture disk from tests/system/mkext2fixture.sh is attached as the
# IDE disk, and the harness kernel boots the test image beside it; the
# probe mounts the fixture's partition and runs its rows.
#
# Usage: ext2.sh KERNEL IMAGE HARNESS DISK MKMBR
#
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; MKMBR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "ext2-test: FAIL: $1" >&2; exit 1; }

sh "$REPO/tests/system/mkext2fixture.sh" "$DISK" "$MKMBR" > /dev/null || fail "the fixture disk"

run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# ---- 1: the walker's link rows on the mounted fixture ----
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "EXT2PROBE OK (links)" \
    --never "EXT2PROBE FAIL" --log "$DISK.links.log" "expect=Type 'help'" \
    "send=exec /ext2probe links" "expect=EXT2PROBE" > /dev/null 2>&1; then
    echo "ext2-test: the symbolic-link and hard-link rows on a mounted volume: OK"
else
    fail "the link rows"
fi
grep -aq 'ext2: mounted: ' "$DISK.links.log" || fail "the mount record"

echo "ext2-test: PASS"
