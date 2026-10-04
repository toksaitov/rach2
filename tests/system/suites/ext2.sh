#!/usr/bin/env sh
#
# ext2.sh: the ext2 driver booted (`tests/run ext2`).
#
# Two disks. The fixture disk from tests/system/mkext2fixture.sh is attached
# as the IDE disk and the harness kernel boots the test image beside it; the
# probe mounts the fixture's partition and runs its rows. The root disk has
# the os-real system on an ext2 first partition and an empty ext2 second
# one; the ordinary kernel boots it as its root with `rach2.root=ext2`,
# writes to it, makes volumes in files with `mke2fs` and `mkfs.vfat`, is
# killed under it, and the boots that follow show the check, the read-only
# fault, recovery's `e2fsck` and the clean boot after.
#
# Usage: ext2.sh KERNEL IMAGE HARNESS DISK MKMBR RECOVERY_KERNEL RECOVERY_IMAGE
#
set -u

# The build tree of the architecture tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4; MKMBR=$5; RKERNEL=$6; RIMAGE=$7
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "ext2-test: FAIL: $1" >&2; exit 1; }

. "$REPO/tools/setup/e2fsprogs_path.sh"

sh "$REPO/tests/system/probe_roots.sh" "$KERNEL" "$DISK.probes" ext2 \
    || fail "the probes on an ext2 root"
# The mappings, whose shared mapping of a file is written back on ext2 and
# refused on the KRFFS root the default suite boots.
sh "$REPO/tests/system/probe_roots.sh" "$KERNEL" "$DISK.maps" ext2 musl/mapmusl \
    || fail "the mappings on an ext2 root"

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

if run --append "rach2.rescue" --timeout 90 --steptmo 60 --ok "EXT2PROBE OK (cost)" \
    --never "EXT2PROBE FAIL" --log "$DISK.cost.log" "expect=Type 'help'" \
    "send=exec /ext2probe cost" "expect=EXT2PROBE" > /dev/null 2>&1; then
    grep -a 'ext2-cost: result:' "$DISK.cost.log" | tr -d '\r'
    echo "ext2-test: small writes share commands across calls: OK"
else
    fail "the cross-call command counts"
fi

# ---- the root disk ----
ROOT="$DISK.root"
VOL="$ROOT.p1"
VOL2="$ROOT.p2"
PASSWORD=$(cat "$REPO/krffs/rach2_demo_password")
sh "$REPO/tools/image/mkext2.sh" "$REPO/krffs/manifests/os-real.manifest" "$VOL" 96 > /dev/null 2>&1 \
    || fail "the os-real ext2 volume"
# The set-id probe, the helper it execs and the probes run beside it are test
# programs and are not in the shipped image, so they are put into this volume
# here rather than into the manifest, each at the root under its own name.
# `write` stores a file with the mode of the host's copy, which is not what
# these need, so each mode is set after it.
for prog in modeprobe suidhelper zombieprobe musl/lchownmusl musl/direntmusl; do
    name=$(basename "$prog")
    debugfs -w -R "write $REPO/$ARCH_BUILD/tests/$prog $name" "$VOL" > /dev/null 2>&1 \
        || fail "staging $name in the ext2 volume"
    debugfs -w -R "sif $name mode 0100755" "$VOL" > /dev/null 2>&1 \
        || fail "the mode of $name in the ext2 volume"
done
rm -f "$VOL2"; : > "$VOL2"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    "$VOL2" 32M > /dev/null 2>&1 || fail "the second volume"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$VOL" "$VOL2"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$ROOT"
"$MKMBR" --lba "$ROOT" 2048:196608:83 198656:65536:83 > /dev/null || fail "the root disk's table"

# The first partition out of the disk and back, for the host's tools.
p1_out() { dd if="$ROOT" of="$VOL" bs=1048576 skip=1 count=96 2>/dev/null; }
p1_in() { dd if="$VOL" of="$ROOT" bs=1048576 seek=1 conv=notrunc 2>/dev/null; }
# The superblock's state field of the first partition, as a number.
p1_state() { od -A n -t u2 -j $((1048576 + 1024 + 58)) -N 2 "$ROOT" | tr -d ' '; }

# A boot of the ordinary kernel on the root disk, logged in as root, with
# the given steps after the prompt; the log is $1. A boot that changes the
# root syncs before its last marker, since ext2 writes back and the harness
# kills the machine at the marker: the root is then left unclean and whole,
# which row 3 checks.
boot_root() {
    log=$1; shift
    "$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$ROOT,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.root=ext2" --timeout 150 --steptmo 40 --log "$log" \
        "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
        "expect=~ #" "$@" > /dev/null 2>&1
}

# ---- 2: the ext2 root, its mount table, its order and a refused write ----
boot_root "$ROOT.first.log" --ok "ROOTDONE" \
    "send=cat /proc/mounts" "expect=/dev/disk0p1 / ext2 rw,relatime 0 0" \
    "send=echo first > /root/first && sync && echo WRITE\"\"OK" "expect=WRITEOK" \
    "send=dd if=/dev/zero of=/dev/disk0p2 bs=512 count=1; echo DD\"\"DONE" "expect=DDDONE" \
    "send=echo ROOT\"\"DONE" "expect=ROOTDONE" || fail "the ext2 root boot"
echo "ext2-test: rach2.root=ext2 mounts the first ext2 partition as the root, writable: OK"

# ---- 2b: what one small write costs the disk, which is entry 49 of milestone
#      50. A 4 KiB rewrite in place is one data write and one inode store, each
#      followed by the drive's cache flush, so four commands. The inode was
#      stored twice a call until `7d94137d`, which made it six. Fifty rewrites
#      are counted through `/proc/disk` and must stay under five a write, so a
#      second store coming back fails this row rather than passing unnoticed.
boot_root "$ROOT.cost.log" --ok "COSTDONE" \
    "send=dd if=/dev/zero of=/root/cost.bin bs=4096 count=1 2>/dev/null && sync && echo MADE\"\"IT" \
    "expect=MADEIT" \
    "send=a=\$(grep ^disk-commands /proc/disk | cut -d' ' -f2); i=0; ok=1; echo COST\"\"START" "expect=COSTSTART" \
    "send=while [ \$i -lt 50 ]; do dd if=/dev/zero of=/root/cost.bin bs=4096 count=1 conv=notrunc 2>/dev/null || ok=0; i=\$((i+1)); done; sync || ok=0; echo COST\"\"WROTE" "expect=COSTWROTE" \
    "send=[ \$(wc -c < /root/cost.bin) -eq 4096 ] || ok=0; b=\$(grep ^disk-commands /proc/disk | cut -d' ' -f2); [ \$ok -eq 1 ] && echo COST=\$((b-a))" \
    "expect=COST=" \
    "send=echo COST\"\"DONE" "expect=COSTDONE" || fail "the small-write cost boot"
COST=$(grep -a "^COST=[0-9]" "$ROOT.cost.log" | head -1 | tr -d '\r' | cut -d= -f2)
[ -n "$COST" ] || fail "no command count for the 4 KiB rewrites"
if [ "$COST" -gt 250 ]; then
    echo "ext2-test: FAIL: 50 rewrites of 4 KiB took $COST disk commands, over the 250 that five a write allows" >&2
    exit 1
fi
echo "ext2-test: a 4 KiB rewrite costs the disk no more than five commands ($COST for 50): OK"

# ---- 2c: the same count for an append, which is the other half of entry 49.
#      A 4 KiB write that grows the file adds the group's bitmap and its
#      descriptor to the rewrite's pair, so ten commands with their flushes
#      where the rewrite has four; the second inode store made it twelve. The
#      bound is eleven a write, which the fix meets at ten and the second store
#      fails at twelve. The file crosses from the direct pointers into the
#      first indirect block on the way, which is a few commands more in the
#      whole fifty and well inside the bound.
boot_root "$ROOT.append.log" --ok "APPENDDONE" \
    "send=rm -f /root/grow.bin; sync; echo READY\"\"NOW" "expect=READYNOW" \
    "send=a=\$(grep ^disk-commands /proc/disk | cut -d' ' -f2); i=0; ok=1; echo APPEND\"\"START" "expect=APPENDSTART" \
    "send=while [ \$i -lt 50 ]; do dd if=/dev/zero bs=4096 count=1 2>/dev/null >> /root/grow.bin || ok=0; i=\$((i+1)); done; sync || ok=0; echo APPEND\"\"WROTE" "expect=APPENDWROTE" \
    "send=[ \$(wc -c < /root/grow.bin) -eq 204800 ] || ok=0; b=\$(grep ^disk-commands /proc/disk | cut -d' ' -f2); [ \$ok -eq 1 ] && echo APPEND=\$((b-a))" \
    "expect=APPEND=" \
    "send=echo APPEND\"\"DONE" "expect=APPENDDONE" || fail "the append cost boot"
APPEND=$(grep -a "^APPEND=[0-9]" "$ROOT.append.log" | head -1 | tr -d '\r' | cut -d= -f2)
[ -n "$APPEND" ] || fail "no command count for the 4 KiB appends"
if [ "$APPEND" -gt 550 ]; then
    echo "ext2-test: FAIL: 50 appends of 4 KiB took $APPEND disk commands, over the 550 that eleven a write allows" >&2
    exit 1
fi
echo "ext2-test: a 4 KiB append costs the disk no more than eleven commands ($APPEND for 50): OK"

# ---- 2d: the set-id rules on this file system, which is entry 48 of milestone
#      50. One plan decides what a chown clears, and both dispatch paths reach
#      it; the probe asserts a directory keeping its bits, a setgid file with
#      and without group-execute, a privileged change, a permitted group
#      change, a refused one and both spellings of a request that changes
#      nothing; a directory, a setgid file with and without group-execute and
#      a privileged change are asserted over both the pathname call and the
#      descriptor, and the rest over the path the caller uses for them. It runs
#      here because the answers must be the file system's, not KRFFS's. The
#      probe empties its own supplementary group list, which is what its
#      escalation row needs.
boot_root "$ROOT.mode.log" --ok "MODESYNCED" \
    "send=/modeprobe /tmp" "expect=MODEPROBE OK" \
    "send=sync; echo MODE\"\"SYNCED" "expect=MODESYNCED" || fail "the set-id rules on the ext2 root"
echo "ext2-test: the chown and write set-id rules hold on an ext2 root: OK"

# ---- 2d': lchown and fchownat's AT_SYMLINK_NOFOLLOW on a file system that has
#      symbolic links. The probe runs in the default suite too, on the KRFFS
#      root, which has none; here its link rows are required: the link changes
#      and its target does not, chown changes the target, an O_PATH descriptor
#      opened with O_NOFOLLOW names the link, and a dangling link changes.
boot_root "$ROOT.lchown.log" --ok "LCHOWNSYNCED" \
    "send=/lchownmusl /tmp" "expect=LCHOWNMUSL OK \(links\)" \
    "send=sync; echo LCHOWN\"\"SYNCED" "expect=LCHOWNSYNCED" || fail "lchown on the ext2 root"
echo "ext2-test: lchown and AT_SYMLINK_NOFOLLOW change a link and chown its target: OK"

# ---- 2d'': the entry types getdents64 reports from ext2's file-type bytes. The
#       probe runs in the default suite too; here a symbolic link must be
#       DT_LNK, and every entry of / and /bin of this root must have the type
#       lstat gives it.
boot_root "$ROOT.dirent.log" --ok "DIRENTSYNCED" \
    "send=/direntmusl /tmp" "expect=DIRENTMUSL OK \(links\)" \
    "send=sync; echo DIRENT\"\"SYNCED" "expect=DIRENTSYNCED" || fail "d_type on the ext2 root"
echo "ext2-test: getdents64 reports each entry's type, a link as DT_LNK: OK"

# ---- 2e: signal teardown beside a writer on an ext2 root. This stress row
# does not force lock contention; exit_hosttest proves the cleanup ordering.
# The boot ends with a poweroff and not a kill: the probe reports before the
# last of its killed processes has given back the files it held unlinked,
# and a kill then would leave their inodes orphaned for the next boot's check,
# where row 3 needs a root the kill of 2f alone left dirty. The poweroff waits
# for every teardown and unmounts the root clean.
boot_root "$ROOT.zombie.log" --ok "Goodbye" \
    "send=cd /tmp && /zombieprobe" "expect=ZOMBIEPROBE OK" \
    "send=cd / && poweroff" "expect=Goodbye" \
    || fail "a process killed while its teardown waits"
grep -aq 'shutdown: root: unmounted' "$ROOT.zombie.log" \
    || fail "the root unmounted after the killed processes"
echo "ext2-test: forty processes killed beside a writer, and the machine goes on and unmounts its root: OK"
ROOT_LINE=$(grep -an 'ext2: root: /dev/disk0p1' "$ROOT.first.log" | head -1 | cut -d: -f1)
USB_LINE=$(grep -an 'usb: started' "$ROOT.first.log" | head -1 | cut -d: -f1)
[ -n "$ROOT_LINE" ] && [ -n "$USB_LINE" ] && [ "$ROOT_LINE" -lt "$USB_LINE" ] \
    || fail "the root is mounted before the USB stack starts"
echo "ext2-test: the root is mounted before the USB stack starts: OK"
grep -aq 'ext2: check' "$ROOT.first.log" && fail "a clean volume was checked"
grep -aq 'Operation not permitted' "$ROOT.first.log" \
    || fail "the ordinary build refuses a write to a partition node"
echo "ext2-test: the ordinary build refuses a write to a partition node: OK"

# ---- 2f: mke2fs and mkfs.vfat make a volume in a file, which a loop device
#      mounts writable; the ordinary kernel writes no disk node, so this is
#      how a file system is made on it. The files are made in the second
#      partition, which is unmounted after, so the host reads them back and
#      the root's blocks the later rows name are untouched ----
boot_root "$ROOT.mkfs.log" --ok "MKFSDONE" \
    "send=mkdir -p /mnt/p2 /mnt/v && mount -t ext2 /dev/disk0p2 /mnt/p2 && mke2fs -q -F -L made /mnt/p2/v.ext2 8M && loopctl attach 0 /mnt/p2/v.ext2 && mount -t ext2 /dev/loop0 /mnt/v && grep loop0 /proc/mounts" \
    "expect=/dev/loop0 /mnt/v ext2 rw" \
    "send=echo made > /mnt/v/f && umount /mnt/v && mount -t ext2 /dev/loop0 /mnt/v && echo GOT=\$(cat /mnt/v/f) && umount /mnt/v && loopctl detach 0" \
    "expect=GOT=made" \
    "send=: > /mnt/p2/v.fat && mkfs.vfat -n RACH2 /mnt/p2/v.fat 14336 && loopctl attach 0 /mnt/p2/v.fat && mount -t vfat /dev/loop0 /mnt/v && grep loop0 /proc/mounts" \
    "expect=/dev/loop0 /mnt/v vfat rw" \
    "send=echo made > /mnt/v/f && umount /mnt/v && loopctl detach 0 && umount /mnt/p2 && sync && echo MKFS\"\"DONE" \
    "expect=MKFSDONE" || fail "mke2fs and mkfs.vfat in a file"
dd if="$ROOT" of="$VOL2" bs=512 skip=198656 count=65536 2>/dev/null
debugfs -R "dump /v.ext2 $ROOT.v.ext2" "$VOL2" > /dev/null 2>&1 || fail "the ext2 volume out of the partition"
debugfs -R "dump /v.fat $ROOT.v.fat" "$VOL2" > /dev/null 2>&1 || fail "the FAT volume out of the partition"
e2fsck -fn "$ROOT.v.ext2" > "$ROOT.fsck0.log" 2>&1 || fail "the host's e2fsck on the volume mke2fs made"
dumpe2fs -h "$ROOT.v.ext2" > "$ROOT.made.log" 2>/dev/null || fail "dumpe2fs on the volume mke2fs made"
grep -q '^Filesystem features: *resize_inode filetype sparse_super large_file$' "$ROOT.made.log" \
    || fail "the features mke2fs gave the volume"
grep -q '^Block size: *4096$' "$ROOT.made.log" || fail "the block size mke2fs gave the volume"
grep -q '^Filesystem volume name: *made$' "$ROOT.made.log" || fail "the label mke2fs gave the volume"
[ "$(mtype -i "$ROOT.v.fat" ::/f 2>/dev/null)" = "made" ] || fail "the file written on the FAT volume"
echo "ext2-test: mke2fs and mkfs.vfat make volumes in files that a loop device mounts writable and the host reads: OK"

# ---- 3: the machine was killed: the dirty root is checked, then unmounted ----
[ "$(p1_state)" = "0" ] || fail "the killed machine left the valid bit set"
boot_root "$ROOT.dirty.log" --ok "Goodbye" \
    "send=echo GOT=\$(cat /root/first)" "expect=GOT=first" \
    "send=cat /proc/mounts" "expect=/dev/disk0p1 / ext2 rw,relatime 0 0" \
    "send=poweroff" "expect=Goodbye" || fail "the boot after the kill"
grep -aq 'ext2: check: clean' "$ROOT.dirty.log" || fail "the check's record"
grep -aq 'shutdown: root: unmounted' "$ROOT.dirty.log" || fail "the unmount at poweroff"
[ "$(p1_state)" = "1" ] || fail "poweroff left the valid bit clear"
p1_out; e2fsck -fn "$VOL" > "$ROOT.fsck1.log" 2>&1 || fail "e2fsck after the checked boot"
echo "ext2-test: a root the kill left dirty is checked, mounts writable and unmounts clean: OK"

# ---- 4: a lost block is kept, the volume writable, the error bit set ----
# A lost block rather than an orphan, because `e2fsck -p` repairs the one
# without a question and leaves the other to a person; the host test has
# the orphan rows.
p1_out
debugfs -w -R 'setb 20000' "$VOL" > /dev/null 2>&1
debugfs -w -R 'ssv state 0' "$VOL" > /dev/null 2>&1
p1_in
boot_root "$ROOT.lost.log" --ok "Goodbye" \
    "send=cat /proc/mounts" "expect=/dev/disk0p1 / ext2 rw,relatime 0 0" \
    "send=echo again > /root/again && sync && echo WRITE\"\"OK" "expect=WRITEOK" \
    "send=poweroff" "expect=Goodbye" || fail "the boot with a lost block"
grep -aq 'ext2: check: 1 lost blocks, 0 orphan inodes, kept for e2fsck' "$ROOT.lost.log" \
    || fail "the lost block's record"
[ "$(p1_state)" = "2" ] || fail "the error bit after the lost block's boot"
p1_out; e2fsck -fn "$VOL" > "$ROOT.fsck2.log" 2>&1; [ $? -eq 4 ] || fail "e2fsck's verdict on the lost block"
grep -q 'Block bitmap differences' "$ROOT.fsck2.log" || fail "the lost block left to e2fsck"
echo "ext2-test: a lost block mounts writable with the error bit set, kept for e2fsck: OK"

# ---- 5: a fault mounts the root read-only ----
debugfs -w -R 'freei /etc/hostname' "$VOL" > /dev/null 2>&1
p1_in
boot_root "$ROOT.fault.log" --ok "Goodbye" \
    "send=cat /proc/mounts" "expect=/dev/disk0p1 / ext2 ro,relatime 0 0" \
    "send=echo x > /root/x" "expect=Read-only file system" \
    "send=poweroff" "expect=Goodbye" || fail "the read-only boot"
grep -aq 'ext2: check: inode [0-9]*: a reached inode the bitmap calls free' "$ROOT.fault.log" \
    || fail "the fault's record"
grep -aq 'ext2: mounted read-only: the check found a fault' "$ROOT.fault.log" || fail "the read-only reason"
echo "ext2-test: a fault mounts the root read-only with the fault named: OK"

# ---- 6: a recovery boot repairs only when asked. Without `rach2.fsck` the
#         boot reaches its login with the disk byte for byte as it was, the
#         dirty root included; with it, init runs e2fsck -p on every ext2
#         partition, and its nodes write ----
BEFORE=$(cksum < "$ROOT")
"$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$ROOT,format=raw,if=ide,index=0,media=disk" \
    --timeout 150 --steptmo 40 --ok "LOOKDONE" --log "$ROOT.look.log" \
    "expect=recovery image" "expect=~ #" \
    "send=echo LOOK\"\"DONE" "expect=LOOKDONE" > /dev/null 2>&1 || fail "the recovery boot without the repair"
[ "$(cksum < "$ROOT")" = "$BEFORE" ] || fail "a recovery boot without rach2.fsck changed the disk"
grep -aq 'init: e2fsck' "$ROOT.look.log" && fail "a recovery boot without rach2.fsck ran e2fsck"
echo "ext2-test: a recovery boot without rach2.fsck leaves the disk as it was: OK"
"$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$ROOT,format=raw,if=ide,index=0,media=disk" --append "rach2.fsck" \
    --timeout 150 --steptmo 40 --ok "RECDONE" --log "$ROOT.recovery.log" \
    "expect=recovery image" "expect=~ #" \
    "send=dd if=/dev/disk0p2 of=/tmp/s bs=512 count=1 skip=16383 && printf 'rach2 wrote this sector%%s' '' | dd of=/tmp/w bs=512 count=1 conv=sync 2>/dev/null && echo SAVED\"\"IT" "expect=SAVEDIT" \
    "send=dd if=/tmp/w of=/dev/disk0p2 bs=512 count=1 seek=16383 && dd if=/dev/disk0p2 bs=512 count=1 skip=16383 | head -c 23 && dd if=/tmp/s of=/dev/disk0p2 bs=512 count=1 seek=16383 && echo BACK\"\"OK" \
    "expect=rach2 wrote this sector" "expect=BACKOK" "send=echo REC\"\"DONE" "expect=RECDONE" > /dev/null 2>&1 || fail "the recovery boot"
grep -aq 'init: e2fsck: /dev/disk0p1: status 1' "$ROOT.recovery.log" || fail "e2fsck's repair of the root"
grep -aq 'init: e2fsck: /dev/disk0p2: status 0' "$ROOT.recovery.log" || fail "e2fsck's pass over the second partition"
p1_out; e2fsck -fn "$VOL" > "$ROOT.fsck3.log" 2>&1 || fail "e2fsck after recovery"
[ "$(p1_state)" = "1" ] || fail "the valid bit after recovery"
[ "$(cksum < "$ROOT")" != "$BEFORE" ] || fail "the repair changed nothing"
echo "ext2-test: recovery's init runs e2fsck -p on every ext2 partition under rach2.fsck, and its nodes write: OK"

# ---- 7: the repaired root boots clean ----
boot_root "$ROOT.clean.log" --ok "Goodbye" \
    "send=cat /proc/mounts" "expect=/dev/disk0p1 / ext2 rw,relatime 0 0" \
    "send=echo GOT=\$(cat /root/again)" "expect=GOT=again" \
    "send=poweroff" "expect=Goodbye" || fail "the clean boot"
grep -aq 'ext2: check' "$ROOT.clean.log" && fail "the repaired root was checked"
echo "ext2-test: the repaired root boots without a check: OK"

echo "ext2-test: PASS"
