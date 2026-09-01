#!/bin/sh
# scratch.sh: the rehearsal of milestone 43's fourth partition, on the sealed
# disk image in QEMU. The recovery ISO boots as a stick beside the disk, the
# procedure in dev-log/37-fatwrite/hardware.md runs over SSH exactly as it will
# on the machine (sector zero saved, rebuilt on the host with the fourth entry,
# written back by reflash, the volume written by reflash and checked), and then
# the ordinary system boots from the disk, mounts the new partition for
# writing, and what it wrote reads back on the host. The QEMU disk is the
# sealed image plus 64 MiB, since the image ends where the recovery partition
# does.
#
# Usage: scratch.sh <sealed-disk> <recovery-iso> <mkfatfixture> [ssh port]
set -u
DISK=$1; ISO=$2; CHECK=$3; PORT="${4:-2299}"
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
WORK="$DISK.scratch"
KNOWN="$WORK.known"
LOG=""
QPID=""
SCRATCH_SECTOR=1290240
SCRATCH_SECTORS=65536
SCRATCH_BYTES=$((SCRATCH_SECTORS * 512))
SCRATCH_OFFSET=$((SCRATCH_SECTOR * 512))

fail() {
    echo "scratch-test: FAIL: $1" >&2
    stop
    exit 1
}
stop() {
    if [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null
        wait "$QPID" 2>/dev/null
    fi
    QPID=""
}
trap 'stop; rm -f "$KNOWN"' EXIT
trap 'stop; exit 130' INT
trap 'stop; exit 143' TERM

machine() {
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o ConnectTimeout=20 -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$REPO/krffs/rach2_demo_ed25519" -p "$PORT" root@localhost "$1" 2>/dev/null
}

# Wait until SSH answers, at most two minutes.
wait_ssh() {
    i=0
    while [ "$i" -lt 60 ]; do
        if [ "$(machine 'echo ALIVE')" = "ALIVE" ]; then
            return 0
        fi
        sleep 2
        i=$((i + 1))
    done
    return 1
}

rm -f "$WORK" "$KNOWN"
cp "$DISK" "$WORK"
dd if=/dev/zero bs=1048576 count=64 >> "$WORK" 2>/dev/null || fail "extending the work disk"

# ---- the stick: the recovery ISO booted beside the disk ----
LOG="$WORK.recovery.log"
qemu-system-i386 -cpu max -display none -rtc base=utc \
    -drive "file=$WORK,format=raw,if=none,id=drv0" \
    -device ich9-ahci,id=ahci0 -device ide-hd,drive=drv0,bus=ahci0.0 \
    -cdrom "$ISO" -boot d \
    -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!
wait_ssh || fail "the recovery stick did not answer SSH"
echo "scratch-test: the recovery stick boots beside the disk and answers SSH: OK"
machine 'reflash 2>&1 | head -1' | grep -q 'usage\|reflash' || fail "reflash is not on the stick"

# ---- 1: save what will be overwritten ----
machine 'dd if=/dev/disk0 bs=512 count=1 2>/dev/null' > "$WORK.sector0" || fail "reading sector 0"
[ "$(wc -c < "$WORK.sector0" | tr -d ' ')" = "512" ] || fail "sector 0 is not 512 bytes"
machine "dd if=/dev/disk0 bs=512 skip=$SCRATCH_SECTOR count=$SCRATCH_SECTORS 2>/dev/null | gzip -c" \
    > "$WORK.before.gz" || fail "reading the range to be overwritten"
[ "$(gunzip -c "$WORK.before.gz" | wc -c | tr -d ' ')" = "$SCRATCH_BYTES" ] \
    || fail "the saved range is not $SCRATCH_BYTES bytes"
echo "scratch-test: sector 0 and every sector about to be written are saved on the host: OK"

# ---- 2: the new sector 0 on the host: the saved one plus the fourth entry ----
"$REPO/build/tools/image/mkmbr" --list "$WORK" > "$WORK.table" || fail "reading the table"
[ "$(wc -l < "$WORK.table" | tr -d ' ')" = "3" ] || fail "the sealed table does not have three entries"
E1=$(awk 'NR == 1 { printf "%d:%d:%s", $3 / 512, $4 / 512, $2 }' "$WORK.table")
E2=$(awk 'NR == 2 { printf "%d:%d:%s", $3 / 512, $4 / 512, $2 }' "$WORK.table")
E3=$(awk 'NR == 3 { printf "%d:%d:%s", $3 / 512, $4 / 512, $2 }' "$WORK.table")
rm -f "$WORK.sparse"
dd if="$WORK.sector0" of="$WORK.sparse" bs=512 count=1 2>/dev/null
dd if=/dev/zero of="$WORK.sparse" bs=512 seek=$((SCRATCH_SECTOR + SCRATCH_SECTORS - 1)) count=1 2>/dev/null
"$REPO/build/tools/image/mkmbr" --lba "$WORK.sparse" "$E1" "$E2" "$E3" "$SCRATCH_SECTOR:$SCRATCH_SECTORS:0c" >/dev/null \
    || fail "mkmbr with the fourth entry"
dd if="$WORK.sparse" of="$WORK.sector0.new" bs=512 count=1 2>/dev/null
"$REPO/build/tools/image/mkmbr" --list "$WORK.sparse" | awk 'NR<=3' > "$WORK.table.new"
awk 'NR<=3' "$WORK.table" | cmp -s - "$WORK.table.new" || fail "the three entries changed"
cmp -s -n 446 "$WORK.sector0" "$WORK.sector0.new" || fail "the boot code changed"
echo "scratch-test: the new sector 0 keeps the code and the three entries and adds the fourth: OK"

# ---- 3: recovery is the saved boot default before any write ----
PREV="$(machine 'bootctl env saved_entry' | sed -n 's/^bootctl: saved_entry: //p' | tr -d '\r')"
[ -n "$PREV" ] || PREV=candidate
OUT="$(machine 'bootctl env saved_entry recovery && bootctl env saved_entry' | tr -d '\r')"
echo "$OUT" | grep -q 'saved_entry: recovery' || fail "recovery is not the saved boot default: $OUT"
echo "scratch-test: recovery is the saved boot default before the first write: OK"

# ---- 4: write sector 0, read it back, undo it from the saved copy, and
#         write it again ----
machine 'reflash /dev/disk0 0 512' < "$WORK.sector0.new" >/dev/null 2>&1 || fail "reflash of sector 0"
machine 'dd if=/dev/disk0 bs=512 count=1 2>/dev/null' > "$WORK.sector0.back" || fail "reading sector 0 back"
cmp -s "$WORK.sector0.new" "$WORK.sector0.back" || fail "sector 0 read back differs"
machine 'reflash /dev/disk0 0 512' < "$WORK.sector0" >/dev/null 2>&1 || fail "reflash of the saved sector 0"
machine 'dd if=/dev/disk0 bs=512 count=1 2>/dev/null' | cmp -s - "$WORK.sector0" \
    || fail "the saved sector 0 did not put the table back"
machine 'reflash /dev/disk0 0 512' < "$WORK.sector0.new" >/dev/null 2>&1 || fail "reflash of sector 0 again"
machine 'dd if=/dev/disk0 bs=512 count=1 2>/dev/null' | cmp -s - "$WORK.sector0.new" \
    || fail "sector 0 read back differs after the undo"
echo "scratch-test: sector 0 written through reflash, read back identical, undone from the saved copy, written again: OK"

# ---- 5: the volume, formatted on the host, checked, written, checked ----
rm -f "$WORK.vol"
mformat -i "$WORK.vol" -C -F -T "$SCRATCH_SECTORS" -v SCRATCH :: || fail "mformat"
"$CHECK" --check "$WORK.vol" >/dev/null || fail "the fresh volume does not pass the check"
gzip -c "$WORK.vol" | machine "gunzip | reflash /dev/disk0 $SCRATCH_OFFSET $SCRATCH_BYTES" >/dev/null 2>&1 \
    || fail "reflash of the volume"
machine "dd if=/dev/disk0 bs=512 skip=$SCRATCH_SECTOR count=64 2>/dev/null" > "$WORK.vol.back" \
    || fail "reading the volume back"
dd if="$WORK.vol" bs=512 count=64 2>/dev/null | cmp -s - "$WORK.vol.back" || fail "the volume's first sectors differ"
stop
"$CHECK" --check "$WORK" "$SCRATCH_SECTOR" >/dev/null || fail "the written volume does not pass the check at its offset"
echo "scratch-test: the volume written through reflash passes the check on the disk: OK"

# ---- 6: the ordinary system boots from the disk, writes the partition,
#         and puts the boot default back ----
LOG="$WORK.system.log"
qemu-system-i386 -cpu max -display none -rtc base=utc \
    -drive "file=$WORK,format=raw,if=none,id=drv0" \
    -device ich9-ahci,id=ahci0 -device ide-hd,drive=drv0,bus=ahci0.0 \
    -netdev "user,id=n0,net=192.168.0.0/24,host=192.168.0.2,hostfwd=tcp::$PORT-:22" \
    -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!
rm -f "$KNOWN"
wait_ssh || fail "the ordinary system did not answer SSH"
machine 'ls /dev/disk0p4' | grep -q disk0p4 || fail "the fourth partition node is absent"
OUT=$(machine 'mkdir -p /mnt && mount -t vfat /dev/disk0p4 /mnt && echo scratch written > /mnt/scratch.txt && umount /mnt && echo WROTE')
[ "$OUT" = "WROTE" ] || fail "the system could not mount and write the partition: $OUT"
OUT="$(machine "bootctl env saved_entry $PREV && bootctl env saved_entry" | tr -d '\r')"
echo "$OUT" | grep -q "saved_entry: $PREV" || fail "the boot default did not go back to $PREV: $OUT"
stop
echo "scratch-test: the ordinary system mounts the new partition, writes it, and puts the boot default back to $PREV: OK"

# ---- 7: what it wrote, read on the host ----
mcopy -i "$WORK@@$SCRATCH_OFFSET" ::/scratch.txt "$WORK.scratch.txt" >/dev/null 2>&1 || fail "mcopy from the partition"
[ "$(cat "$WORK.scratch.txt")" = "scratch written" ] || fail "the file's bytes differ through mtools"
"$CHECK" --check "$WORK" "$SCRATCH_SECTOR" >/dev/null || fail "the partition is not clean after the write"
echo "scratch-test: the file reads back through mtools and the partition is clean: OK"
echo "scratch-test: PASS"
