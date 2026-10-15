#!/bin/sh
# exfat.sh: writing exFAT, proved from a program, across a reboot, by the
# host's reader and the external checker where the host has one, through a
# crash, from BusyBox, and under a lag with two writers. Six boots on one disk
# whose second partition is an exFAT volume mkexfatfixture built empty.
#
# Usage: exfat.sh KERNEL IMAGE HARNESS DISK OSREAL PASSWORD MKEXFATFIXTURE
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; OSREAL=$5; PASSWORD=$6; MKFIX=$7
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
PART=100352
SECTORS=32768

fail() { echo "exfat-test: FAIL: $1" >&2; exit 1; }

sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DISK" 80 48 >/dev/null 2>&1 \
    || fail "mkdisk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:98304:7f $PART:$SECTORS:07 >/dev/null || fail "mkmbr"
"$MKFIX" "$DISK.vol" --empty || fail "the empty volume"
dd if="$DISK.vol" of="$DISK" bs=512 seek=$PART conv=notrunc 2>/dev/null || fail "placing the volume"

# run ARG...: boot the test image on the disk with the harness's ARGs.
run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# walk_clean WHAT: walk the volume with the host's reader, and fail with WHAT
# on any finding but the line LOST has. The crash may leave lost clusters,
# which the mount's check counts and leaves for a host tool; walk_repaired
# records that one line in LOST, and from then on it is the only finding a
# walk may show.
LOST=""
walk_clean() {
    "$MKFIX" --check "$DISK" $PART > "$DISK.check" 2>&1 && return 0
    # A walk that found faults ends with `check: FAIL: `; any other is a crash.
    grep -q '^check: FAIL: ' "$DISK.check" || { cat "$DISK.check" >&2; fail "the reader did not walk the volume"; }
    if grep '^check:' "$DISK.check" | grep -v '^check: FAIL:' \
        | grep -vxF "${LOST:-no such line}" | grep -q .; then
        cat "$DISK.check" >&2
        fail "$1"
    fi
}
# walk_repaired WHAT: walk the volume, keep its lost-cluster line in LOST, and
# judge it as walk_clean does.
walk_repaired() {
    "$MKFIX" --check "$DISK" $PART > "$DISK.check" 2>&1 || true
    LOST="$(grep '^check: lost:' "$DISK.check" || true)"
    walk_clean "$1"
}

# external_check WHAT LOST: the external checker, where the host has one,
# over a copy of the volume: macOS's fsck_exfat or exfatprogs' fsck.exfat. It
# reports without modifying, and must call the copy clean or the suite fails
# with WHAT. LOST not empty says the volume holds the lost clusters the
# mount's check counts and leaves; the checker then repairs the copy and must
# call the repaired copy clean. The two checkers differ on whether a lost
# cluster is reported before the repair, so that report is not required.
# Without a checker the function does nothing.
external_check() {
    dd if="$DISK" of="$DISK.vol" bs=512 skip=$PART count=$SECTORS 2>/dev/null \
        || fail "copying the volume for the external checker"
    if command -v fsck_exfat >/dev/null 2>&1 && command -v hdiutil >/dev/null 2>&1; then
        DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$DISK.vol" 2>/dev/null \
            | awk 'NR == 1 {print $1}')
        [ -n "$DEV" ] || fail "attaching the volume for fsck_exfat"
        fsck_exfat -n "$DEV" > "$DISK.fsck" 2>&1; rc=$?
        if [ -n "$2" ]; then
            fsck_exfat -y "$DEV" >> "$DISK.fsck" 2>&1
            fsck_exfat -n "$DEV" >> "$DISK.fsck" 2>&1; rc=$?
        fi
        hdiutil detach "$DEV" >/dev/null 2>&1
        [ "$rc" -eq 0 ] || { cat "$DISK.fsck" >&2; fail "$1"; }
    elif command -v fsck.exfat >/dev/null 2>&1; then
        fsck.exfat -n "$DISK.vol" > "$DISK.fsck" 2>&1; rc=$?
        if [ -n "$2" ]; then
            fsck.exfat -y "$DISK.vol" >> "$DISK.fsck" 2>&1
            fsck.exfat -n "$DISK.vol" >> "$DISK.fsck" 2>&1; rc=$?
        fi
        [ "$rc" -eq 0 ] || { cat "$DISK.fsck" >&2; fail "$1"; }
    else
        return 0
    fi
    if [ -n "$2" ]; then
        echo "exfat-test: the external checker calls its repaired copy of the crashed volume clean: OK"
    else
        echo "exfat-test: the external checker calls the volume clean: OK"
    fi
}

# ---- 1: written from a program and unmounted cleanly ----
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (wrote)" \
    --never "FATWRITE FAIL" --log "$DISK.write.log" \
    "expect=Type 'help'" "send=exec /fatwriteprobe write exfat" "expect=FATWRITE" > /dev/null 2>&1; then
    echo "exfat-test: a program writes the volume and unmounts it: OK"
else
    fail "the write boot"
fi
walk_clean "the volume is not consistent after the write boot"
echo "exfat-test: the host's reader finds the written volume consistent: OK"
external_check "the external checker finds a fault after the write boot" ""

# ---- 2: the host reads what was written ----
"$MKFIX" --check "$DISK" $PART --read /pattern.bin > "$DISK.pattern" 2>/dev/null || fail "reading the pattern file"
python3 - "$DISK.pattern" <<'PY' || fail "the pattern file's bytes through the host reader"
import sys
b = open(sys.argv[1], 'rb').read()
sys.exit(0 if len(b) == 65536 and all(b[i] == ((i * 31 + 7) & 0xFF) for i in range(65536)) else 1)
PY
"$MKFIX" --check "$DISK" $PART --list > "$DISK.listing" 2>/dev/null
grep -q '^check: ' "$DISK.listing" || fail "the host reader did not list the volume"
python3 -c '
import sys
for line in sys.stdin:
    if not line.startswith("check: entry "):
        continue
    f = line[len("check: entry "):].rstrip("\n").split(" ")
    if f[-1] == "nofat":
        f = f[:-1]
    print(" ".join(f[:-3]))
' < "$DISK.listing" > "$DISK.names"
for name in "/pattern.bin" "/Long Name File.txt" "/Sub Dir" "/renamed.txt" "/synced.txt"; do
    grep -qxF "$name" "$DISK.names" || fail "the host reader does not list $name"
done
grep -qF "gone.txt" "$DISK.names" && fail "the host reader lists the removed file"
echo "exfat-test: the host reads the bytes and lists the names this driver wrote: OK"

# ---- 3: read back after a reboot ----
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (read back)" \
    --never "FATWRITE FAIL" "expect=Type 'help'" "send=exec /fatwriteprobe readback exfat" \
    "expect=FATWRITE" > /dev/null 2>&1; then
    echo "exfat-test: every byte and name reads back after a reboot: OK"
else
    fail "the read-back boot"
fi

# ---- 4: a crash in the middle of a write sequence, then the mount's check ----
run --append "rach2.rescue" --timeout 60 --steptmo 30 --ok "FATWRITE WRITING" \
    --never "FATWRITE FAIL" "expect=Type 'help'" "send=exec /fatwriteprobe crash exfat" \
    "expect=FATWRITE WRITING" > /dev/null 2>&1 || fail "the crash boot"
"$MKFIX" --check "$DISK" $PART --dump > "$DISK.crashed" 2>&1
grep -q 'dump: dirty 1' "$DISK.crashed" || fail "the killed machine left the volume marked clean"
echo "exfat-test: the machine killed mid-sequence left the dirty bit set: OK"
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (repaired)" \
    --never "FATWRITE FAIL" --log "$DISK.repair.log" "expect=Type 'help'" \
    "send=exec /fatwriteprobe repair exfat" "expect=FATWRITE" > /dev/null 2>&1 \
    && grep -aq 'exfat: check: the volume was not unmounted cleanly' "$DISK.repair.log" \
    && grep -aq 'exfat: check: consistent' "$DISK.repair.log"; then
    echo "exfat-test: the next mount checks the volume and records the check: OK"
else
    fail "the repair boot"
fi
walk_repaired "the volume is not consistent after the repair"
echo "exfat-test: the host's reader finds the repaired volume consistent, lost clusters aside: OK"

# ---- 5: BusyBox writes through the volume ----
LOG="$DISK.busybox.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --timeout 240 --steptmo 60 --ok "BBDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=cp /bin/busybox /mnt/bb && mkdir /mnt/d && mv /mnt/bb '/mnt/d/Busy Box' && echo hi > /mnt/d/h.txt && rm /mnt/renamed.txt && cmp '/mnt/d/Busy Box' /bin/busybox && echo SAME\"\"BYTES" \
    "expect=SAMEBYTES" "send=umount /mnt && echo UN\"\"MOUNTED" \
    "expect=UNMOUNTED" "send=echo BB\"\"DONE" "expect=BBDONE" > /dev/null 2>&1; then
    echo "exfat-test: BusyBox copies, moves, writes and removes on the volume: OK"
else
    fail "the BusyBox boot"
fi
walk_clean "the volume is not consistent after BusyBox"
"$MKFIX" --check "$DISK" $PART --read "/d/Busy Box" > "$DISK.bb" 2>/dev/null || fail "reading the copied program"
"$REPO/build/krffs/readfile.krffs" "$OSREAL" /bin/busybox "$DISK.bb.want" >/dev/null 2>&1 || fail "readfile of busybox"
cmp -s "$DISK.bb" "$DISK.bb.want" || fail "the program BusyBox copied differs through the host reader"
echo "exfat-test: the program BusyBox copied reads identically on the host: OK"

# ---- 6: two writers and a sync at once, under the lag ----
if run --append "rach2.blocklag=3 rach2.rescue" --timeout 180 --steptmo 120 \
    --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe writers exfat" "expect=LAGPROBE" > /dev/null 2>&1; then
    echo "exfat-test: two writers and a sync interleave under the lag and each file is whole: OK"
else
    fail "the writers boot"
fi
walk_clean "the volume is not consistent after the writers"
external_check "the external checker's answer at the end" "$LOST"

if command -v fsck_exfat >/dev/null 2>&1 || command -v fsck.exfat >/dev/null 2>&1; then
    echo "exfat-test: PASS (with the external checker)"
else
    echo "exfat-test: PASS (no external checker; the host's reader judged the volume)"
fi
