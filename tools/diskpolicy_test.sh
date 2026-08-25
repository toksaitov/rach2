#!/usr/bin/env sh

#
# diskpolicy_test.sh: the read-only kernel does not write the disk.
#
# Milestone 24 has Rach2 to a laptop whose SSD has a Windows installation
# its owner has not backed up, and the promise made about it is absolute:
# nothing this kernel does writes that disk until the owner has approved a
# carved partition. The promise is kept by a build switch, so what has to be
# shown is not that a function returns an error but that the platter is not
# touched, and the only way to show that is to look at the bytes.
#
# So this boots the same disk twice, once with each kernel, and compares the
# image afterwards. The read-write boot MUST change it: that is what makes the
# other half meaningful, since a test where neither kernel wrote would pass
# while proving nothing. The read-only boot must refuse a command by name and
# leave the image byte for byte as it was.
#
# Usage: diskpolicy_test.sh <read-write kernel> <read-only kernel>
#

set -e

RW_KERNEL="$1"
RO_KERNEL="$2"

if [ -z "$RW_KERNEL" ] || [ -z "$RO_KERNEL" ]; then
    echo "usage: diskpolicy_test.sh <read-write kernel> <read-only kernel>" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$REPO/build/images/diskpolicy"
BASE="$WORK.base.disk"
RW="$WORK.rw.disk"
RO="$WORK.ro.disk"
RWLOG="$WORK.rw.log"
ROLOG="$WORK.ro.log"

cleanup() {
    rm -f "$BASE" "$RW" "$RO" "$RWLOG" "$ROLOG"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    echo "diskpolicy-test: FAIL: $1" >&2
    exit 1
}

# The artifacts first: a kernel that is not the policy it claims makes every
# comparison below meaningless.
python3 "$REPO/tools/inspect/check_diskpolicy.py" "$RW_KERNEL" read-write || fail "the read-write kernel is not read-write"
python3 "$REPO/tools/inspect/check_diskpolicy.py" "$RO_KERNEL" read-only || fail "the read-only kernel is not read-only"

# And the check's negative half, which is the case that matters: the Readme
# records that the metal image was once built around the ordinary kernel, and
# the only thing standing between that mistake and a stick that can write
# somebody's SSD is this check refusing. A check that is only ever run on the
# artifact it approves has never been shown to reject anything.
if python3 "$REPO/tools/inspect/check_diskpolicy.py" "$RW_KERNEL" read-only >/dev/null 2>&1; then
    fail "the policy check accepted the read-write kernel as read-only"
fi
if python3 "$REPO/tools/inspect/check_diskpolicy.py" "$RO_KERNEL" read-write >/dev/null 2>&1; then
    fail "the policy check accepted the read-only kernel as read-write"
fi
if python3 "$REPO/tools/inspect/check_diskpolicy.py" /dev/null read-only >/dev/null 2>&1; then
    fail "the policy check accepted an empty file"
fi
echo "diskpolicy-test: the policy check rejects each kernel offered as the other: OK"

# A disk built here and never booted, so the comparison starts from a known
# image, not from whatever a previous test left.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$BASE" 64 48 >/dev/null 2>&1 \
    || fail "could not build the disk image"

boot() {
    cp "$BASE" "$2"
    python3 "$REPO/tools/harness.py" --kernel "$1" \
        --drive "file=$2,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.fsdisk rach2.rescue" \
        --timeout 90 --log "$3" --ok "Type 'help'" \
        "expect=Type 'help'" wait=3 >/dev/null 2>&1 || true
}

# The read-write kernel: it mounts, and mounting stamps the dirty flag, so the
# image must differ afterwards. This is the control.
boot "$RW_KERNEL" "$RW" "$RWLOG"
if ! grep -q "OK (KRFFS" "$RWLOG"; then
    fail "the read-write kernel did not mount the disk, so the comparison would prove nothing"
fi
if cmp -s "$BASE" "$RW"; then
    fail "the read-write kernel left the disk unchanged, so this test cannot tell the two apart"
fi
echo "diskpolicy-test: the ordinary kernel mounted the disk and wrote to it: OK"

# The read-only kernel: the same boot, the same disk, and not one byte.
boot "$RO_KERNEL" "$RO" "$ROLOG"
if ! grep -q "disk policy: read-only" "$ROLOG"; then
    fail "the read-only kernel did not report its policy"
fi
if ! grep -q "refused command" "$ROLOG"; then
    fail "the read-only kernel refused nothing, so it never reached the write it should have refused"
fi
if ! cmp -s "$BASE" "$RO"; then
    fail "the read-only kernel changed the disk image"
fi
echo "diskpolicy-test: the read-only kernel refused the write and left the disk byte for byte: OK"
echo "diskpolicy-test: $(grep -m1 'refused command' "$ROLOG")"
echo "diskpolicy-test: PASS"
