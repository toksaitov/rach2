#!/bin/sh
# fatwrite.sh: writing FAT32, proved from a program, across a reboot, by the
# host's tools, through a crash, from BusyBox, and under a lag with two
# writers. Six boots on one disk whose second partition is a FAT32 volume
# mtools formatted.
#
# Usage: fatwrite.sh KERNEL IMAGE HARNESS DISK OSREAL PASSWORD MKFIXTURE
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; OSREAL=$5; PASSWORD=$6; MKFIXTURE=$7
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
VOL="$DISK"@@51380224

fail() { echo "fatwrite-test: FAIL: $1" >&2; exit 1; }

sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DISK" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:98304:7f 100352:28672:0c >/dev/null || fail "mkmbr"
mformat -i "$VOL" -F -T 28672 -v M37FAT :: || fail "mformat"

run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# The volume walked on the host. The crash may leave lost clusters, which the
# mount's check counts and leaves for a host tool; the repair step records
# that one line, and from then on it is the only finding a walk may show.
LOST=""
walk_clean() {
    "$MKFIXTURE" --check "$DISK" 100352 > "$DISK.check" 2>&1 && return 0
    # A checker that crashed or found nothing to say is a failure of its own:
    # its last line names the count of findings.
    grep -q '^check: FAIL: ' "$DISK.check" \
        || { cat "$DISK.check" >&2; fail "the checker did not walk the volume"; }
    if grep '^check:' "$DISK.check" | grep -v '^check: FAIL:\|^check: dirty:' \
        | grep -vxF "${LOST:-no such line}" | grep -q .; then
        cat "$DISK.check" >&2
        fail "$1"
    fi
}
walk_repaired() {
    "$MKFIXTURE" --check "$DISK" 100352 > "$DISK.check" 2>&1 || true
    LOST="$(grep '^check: lost:' "$DISK.check" || true)"
    walk_clean "$1"
}


# ---- 1: written from a program and unmounted cleanly ----
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (wrote)" \
    --never "FATWRITE FAIL" --log "$DISK.write.log" \
    "expect=Type 'help'" "send=exec /fatwriteprobe write" "expect=FATWRITE" > /dev/null 2>&1; then
    echo "fatwrite-test: a program writes the volume and unmounts it: OK"
else
    fail "the write boot"
fi
walk_clean "the volume is not consistent after the write boot"
echo "fatwrite-test: the host's walk finds the written volume consistent: OK"

# ---- 2: the host reads what was written, with mtools ----
mcopy -n -i "$VOL" ::/pattern.bin "$DISK.pattern" >/dev/null 2>&1 || fail "mcopy of the pattern file"
python3 - "$DISK.pattern" <<'PY' || fail "the pattern file's bytes through mtools"
import sys
b = open(sys.argv[1], 'rb').read()
sys.exit(0 if len(b) == 65536 and all(b[i] == ((i * 31 + 7) & 0xFF) for i in range(65536)) else 1)
PY
mdir -i "$VOL" -b :: 2>/dev/null | tr -d '\r' | sort > "$DISK.names"
for name in "::/pattern.bin" "::/Long Name File.txt" "::/Sub Dir/" "::/renamed.txt" "::/synced.txt"; do
    grep -qxF "$name" "$DISK.names" || fail "mtools does not list $name"
done
grep -qF "gone.txt" "$DISK.names" && fail "mtools lists the removed file"
echo "fatwrite-test: mtools reads the bytes and lists the names this driver wrote: OK"
if command -v fsck.fat >/dev/null 2>&1; then
    dd if="$DISK" of="$DISK.vol" bs=512 skip=100352 count=28672 2>/dev/null
    fsck.fat -n "$DISK.vol" >/dev/null 2>&1 || fail "fsck.fat finds a fault"
    echo "fatwrite-test: fsck.fat -n calls the volume clean: OK"
fi

# ---- 3: read back after a reboot ----
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (read back)" \
    --never "FATWRITE FAIL" --log "$DISK.readback.log" \
    "expect=Type 'help'" "send=exec /fatwriteprobe readback" \
    "expect=FATWRITE" > /dev/null 2>&1; then
    echo "fatwrite-test: every byte and name reads back after a reboot: OK"
else
    fail "the read-back boot"
fi

# ---- 4: a crash in the middle of a write sequence, then the mount's check ----
run --append "rach2.rescue" --timeout 60 --steptmo 30 --ok "FATWRITE WRITING" \
    --never "FATWRITE FAIL" --log "$DISK.crash.log" \
    "expect=Type 'help'" "send=exec /fatwriteprobe crash" \
    "expect=FATWRITE WRITING" > /dev/null 2>&1 || fail "the crash boot"
"$MKFIXTURE" --check "$DISK" 100352 > "$DISK.crashed" 2>&1
grep -q 'check: dirty:' "$DISK.crashed" || fail "the killed machine left the volume marked clean"
echo "fatwrite-test: the machine killed mid-sequence left the flag set: OK"
if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "FATWRITE OK (repaired)" \
    --never "FATWRITE FAIL" --log "$DISK.repair.log" "expect=Type 'help'" \
    "send=exec /fatwriteprobe repair" "expect=FATWRITE" > /dev/null 2>&1 \
    && grep -aq 'vfat: check: the volume was not unmounted cleanly' "$DISK.repair.log" \
    && grep -aq 'vfat: check: consistent' "$DISK.repair.log"; then
    echo "fatwrite-test: the next mount checks the volume and records the check: OK"
else
    fail "the repair boot"
fi
walk_repaired "the volume is not consistent after the repair"
echo "fatwrite-test: the host's walk finds the repaired volume consistent, lost clusters aside: OK"

# ---- 5: BusyBox writes through the volume; and a 4 MiB write costs the
#         disk a bounded number of commands. The FAT entries of a run are
#         written through one block at a time: 4 MiB is 8192 clusters of
#         512 bytes on this volume, whose entries fill eight 4 KiB FAT
#         blocks per copy, beside 1024 data blocks written and, since the
#         cache fills a block before it is zeroed, read once each; the count
#         here is under 4000. An entry written through on its own costs
#         16384 commands more. The removal frees the chain the same way. ----
LOG="$DISK.busybox.log"
if "$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max --append "rach2.diskdiag" \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --timeout 240 --steptmo 60 --ok "BBDONE" --never "DDFAILED" --never "BIGFAILED" --never "COUNTFAILED" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mkdir /mnt && mount -t vfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED" \
    "expect=MOUNTED" "send=cp /bin/busybox /mnt/bb && mkdir /mnt/d && mv /mnt/bb '/mnt/d/Busy Box' && echo hi > /mnt/d/h.txt && rm /mnt/renamed.txt && cmp '/mnt/d/Busy Box' /bin/busybox && echo SAME\"\"BYTES" \
    "expect=SAMEBYTES" "send=A=\$(sed -n 's/^disk-commands //p' /proc/disk); dd if=/dev/zero of=/mnt/big.bin bs=64k count=64 2>/dev/null && sync && echo DD\"\"OK || echo DD\"\"FAILED" \
    "expect=DDOK" "send=if [ \"\$(stat -c %s /mnt/big.bin)\" = 4194304 ]; then B=\$(sed -n 's/^disk-commands //p' /proc/disk); echo BIG\"\"WRITTEN; else echo BIG\"\"FAILED; fi" \
    "expect=BIGWRITTEN" "send=if rm /mnt/big.bin && sync && [ ! -e /mnt/big.bin ]; then C=\$(sed -n 's/^disk-commands //p' /proc/disk); echo COMMANDS \$((B-A)) \$((C-B)); echo COUNT\"\"ED; else echo COUNT\"\"FAILED; fi" \
    "expect=COUNTED" "send=umount /mnt && echo UN\"\"MOUNTED" \
    "expect=UNMOUNTED" "send=echo BB\"\"DONE" "expect=BBDONE" > /dev/null 2>&1; then
    echo "fatwrite-test: BusyBox copies, moves, writes and removes on the volume: OK"
else
    fail "the BusyBox boot"
fi
WROTE="$(grep -a '^COMMANDS [0-9]' "$LOG" | tail -1 | tr -d '\r' | awk '{print $2}')"
FREED="$(grep -a '^COMMANDS [0-9]' "$LOG" | tail -1 | tr -d '\r' | awk '{print $3}')"
if [ -n "$WROTE" ] && [ "$WROTE" -gt 0 ] && [ "$WROTE" -lt 5000 ] && [ -n "$FREED" ] && [ "$FREED" -gt 0 ] && [ "$FREED" -lt 256 ]; then
    echo "fatwrite-test: a 4 MiB write costs the disk $WROTE commands and its removal $FREED, the FAT written by blocks: OK"
else
    fail "a 4 MiB write cost the disk ${WROTE:-?} commands and its removal ${FREED:-?}; the FAT is written an entry at a time"
fi
walk_clean "the volume is not consistent after BusyBox"
mcopy -n -i "$VOL" "::/d/Busy Box" "$DISK.bb" >/dev/null 2>&1 || fail "mcopy of the copied program"
"$REPO/build/krffs/readfile.krffs" "$OSREAL" /bin/busybox "$DISK.bb.want" >/dev/null 2>&1 || fail "readfile of busybox"
cmp -s "$DISK.bb" "$DISK.bb.want" || fail "the program BusyBox copied differs through mtools"
echo "fatwrite-test: the program BusyBox copied reads identically through mtools: OK"

# ---- 6: two writers and a sync at once, under the lag ----
if run --append "rach2.blocklag=3 rach2.rescue" --timeout 180 --steptmo 120 \
    --ok "LAGPROBE OK" --never "LAGPROBE FAIL" --log "$DISK.writers.log" \
    "expect=Type 'help'" "send=exec /lagprobe writers" "expect=LAGPROBE" > /dev/null 2>&1; then
    echo "fatwrite-test: two writers and a sync interleave under the lag and each file is whole: OK"
else
    fail "the writers boot"
fi
walk_clean "the volume is not consistent after the writers"

if command -v fsck.fat >/dev/null 2>&1; then
    echo "fatwrite-test: PASS (with fsck.fat)"
else
    echo "fatwrite-test: PASS (fsck.fat absent; the walk and mtools judged the volume)"
fi
