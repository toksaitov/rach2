#!/bin/sh
# rehearse_sitting.sh STICK DISK HARNESS DIRECTORY [SECTORS]: rehearse the
# Lenovo's install sitting in QEMU at the machine's own size, run by hand.
#
# The target is a disk of SECTORS sectors, 488397168 by default, the Lenovo's,
# as a sparse base under a qcow2 overlay, so that `qemu-img map` afterwards
# lists exactly the sectors the boots wrote. The save set is computed by
# `growset` from the head of DISK's ext2 root and the root's offset, with
# DISK's own extent. The recovery system on STICK saves those ranges onto a
# save disk, installs DISK from STICK with the root grown to the target's
# end, and puts the ranges back with `reflash`; every sector the install and
# the restore wrote must lie in the save set, and each range must read back
# as the bytes saved. The times are printed against the sitting's budget of
# ten minutes, which is the machine's and not the emulator's. At the
# Lenovo's size the run takes most of an hour.
#
# STICK is the product's install stick and DISK the sealed disk on it, as
# `make rach2-install-stick` builds them under build/images, the names with
# `-with-key` when the recovery system has a key. The case
# machine.installer-rehearse.sitting, of
# tests/cases/machine.installer-rehearse.json, runs the same steps on a target
# of 524288 sectors with the test stick, and so does this script given the
# test stick, its disk and that size. The work goes in DIRECTORY, which the
# run removes when it passes and keeps, named on its last line, when it fails.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
    echo 'rehearse-sitting: usage: rehearse_sitting.sh STICK DISK HARNESS DIRECTORY [SECTORS]' >&2
    exit 2
fi
STICK=$1
DISK=$2
HARNESS=$3
R=$4
SECTORS=${5:-488397168}
GROWSET="$repo/build/tools/inspect/growset"
NEWPASS=an-installed-machine
USERNAME=alice
USERPASS=alices-own-password

fail=0
note() { echo "rehearse-sitting: $1"; }
bad() {
    echo "rehearse-sitting: FAIL: $1" >&2
    fail=1
}
die() {
    bad "$1"
    exit 1
}
# The images are removed on every exit, and the directory too once the run
# has passed; a failed run keeps its logs there as its evidence.
finish() {
    status=$?
    trap - EXIT
    rm -f "$R/base.raw" "$R/disk.qcow2" "$R/save-stick.img" "$R/product-stick.img"
    if [ "$status" -eq 0 ] && [ "$fail" -eq 0 ]; then
        rm -rf "$R"
        echo "rehearse-sitting: result: passed"
    else
        echo "rehearse-sitting: result: failed: evidence: $R" >&2
        [ "$status" -ne 0 ] || status=1
    fi
    exit "$status"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# A machine left running by an earlier run holds the images this one writes,
# and QEMU refuses to open a file another process has locked.
for left in $(pgrep -f "$R/" 2>/dev/null || true); do
    kill "$left" 2>/dev/null || true
done
rm -rf "$R"
mkdir -p "$R"

# A field of entry N of a disk image's table, in sectors: its first sector,
# field 3. A GUID table, which a protective MBR's type 0xEE in its first
# entry says the image has, is read by `mkgpt --list` and an MBR by
# `mkmbr --list`; both print the extent in bytes.
entry_field() { # entry_field IMAGE N FIELD
    if [ "$(od -An -tx1 -j 450 -N 1 "$1" | tr -d ' ')" = ee ]; then
        "$repo/build/tools/image/mkgpt" --list "$1"
    else
        "$repo/build/tools/image/mkmbr" --list "$1"
    fi | awk -v n="$2" -v f="$3" '$1 == n { printf "%d\n", $f / 512; exit }'
}

IMAGE_BYTES=$(wc -c < "$DISK" | tr -d ' ')
IMAGE_SECTORS=$((IMAGE_BYTES / 512))
# The root's entry in the image's table, the third: its first sector from
# the image's GUID table.
ROOT_START=$(entry_field "$DISK" 3 3)
[ -n "$ROOT_START" ] && [ "$ROOT_START" -gt 0 ] || die "the image's third entry has no start"
dd if="$DISK" of="$R/head.vol" bs=512 skip="$ROOT_START" count=$((128 * 2048)) 2>/dev/null \
    || die "the root's head could not be read from the image"
# The root grows to the sector before the GUID table's backup, 33 sectors
# short of the disk's end.
"$GROWSET" --lba "$ROOT_START" "$R/head.vol" $((SECTORS - 33 - ROOT_START)) > "$R/save.set" \
    || die "growset did not compute the save set"
# The ranges to save, `START COUNT` in sectors: the image's extent, then the
# grow's ranges beyond it; a range inside the extent is saved with it. The
# image's GUID table has its backup in the disk's last 33 sectors once the
# install has moved it there, and the grow writes it there again, so the end
# of the disk is a range of its own. It starts at the eight-sector boundary
# at or below the backup, since the overlay's map reports a write in clusters
# of eight sectors and the checks below accept a written extent only inside
# the saved ranges.
BACKUP_FROM=$(( (SECTORS - 33) / 8 * 8 ))
{
    echo "0 $IMAGE_SECTORS"
    awk -v n="$IMAGE_SECTORS" -v b="$BACKUP_FROM" '$1 == "range" && $2 + $3 > n {
        s = $2 < n ? n : $2; e = $2 + $3 < b ? $2 + $3 : b; if (e > s) print s, e - s }' "$R/save.set"
    echo "$BACKUP_FROM $((SECTORS - BACKUP_FROM))"
} > "$R/ranges"
NRANGES=$(grep -c . "$R/ranges")
note "the save set is the image's $IMAGE_SECTORS sectors, $((NRANGES - 2)) ranges of the grow beyond it and the GUID table's backup"
# The base and the overlay; the backing name is relative to the overlay's
# own directory, which is where both live. Each range's first sector on the
# base names the range, so a save that read another offset, or a restore
# that put zeros back, shows in the digests the host expects below.
dd if=/dev/zero of="$R/base.raw" bs=512 seek=$((SECTORS - 1)) count=1 2>/dev/null || bad "the base disk"
while read -r s c; do
    printf 'range %s %s\n' "$s" "$c" | dd of="$R/base.raw" bs=512 seek="$s" count=1 conv=notrunc 2>/dev/null ||
        bad "the range at $s could not be marked on the base disk"
done < "$R/ranges"
# Clusters of 4 KiB, so the overlay's map is exact to the eight sectors the
# grow's ranges are aligned to; the default 64 KiB would show every range
# rounded up to a cluster and call the rounding a write outside the set.
(cd "$R" && qemu-img create -q -f qcow2 -o cluster_size=4096 -b base.raw -F raw disk.qcow2) || bad "the overlay"
# The save disk: an exFAT volume with room for every range, the format the
# sitting's stick has, written by the machine; the list of ranges goes on it
# as its one file, one `START COUNT` a line, so the machine's own loop reads
# it and the harness types one line. It is on IDE beside the target and not
# on USB as the sitting's stick is, since the rehearsal is of the procedure
# and its set and not of the emulator's buses; the loops find it as the
# second fixed disk. It is not FAT32: this kernel writes FAT32 through a
# block at a time, 0.6 MB a second in the emulator.
SAVE_SECTORS=$(( (IMAGE_SECTORS / 2048 + 2048 + 32) * 2048 ))
"$repo/build/tests/host/mkexfatfixture" --make "$R/save.vol" --sectors "$SAVE_SECTORS" --offset 2048 \
    --add "ranges=$R/ranges" > /dev/null || bad "the save disk's volume"
{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat "$R/save.vol"; } > "$R/save-stick.img"
rm -f "$R/save.vol"
"$repo/build/tools/image/mkmbr" --lba "$R/save-stick.img" 2048:$SAVE_SECTORS:07 > /dev/null || bad "the save disk's table"
cp "$STICK" "$R/product-stick.img" || bad "the product stick's copy"
# A boot of the recovery system from the product stick, with the overlay disk
# on IDE as disk0, the save disk on IDE as disk1 and the product stick disk2,
# which is the order this recovery kernel registers them in; the loops look
# the target up as the first fixed disk and the save disk up as the second
# rather than trusting the numbers. The steps come from a file, one a line.
# rehearse_boot LOG TIMEOUT OK STEPFILE: one boot of the recovery system with
# the disks above, its steps read from STEPFILE one a line and typed in single
# quotes, since the loops carry the machine's variables, which the host's
# shell must not expand. The command line goes to LOG.sh for the record.
rehearse_boot() {
    {
        printf '"%s" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512' "$HARNESS"
        printf ' --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0'
        printf ' --drive "if=none,id=stick,format=raw,file=%s/product-stick.img"' "$R"
        printf ' --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0'
        printf ' --qemu-arg=-drive --qemu-arg="file=%s/disk.qcow2,format=qcow2,if=ide,index=0,media=disk"' "$R"
        printf ' --qemu-arg=-drive --qemu-arg="file=%s/save-stick.img,format=raw,if=ide,index=1,media=disk"' "$R"
        printf ' --timeout %s --steptmo %s --ok "%s" --log "%s"' "$2" "$2" "$3" "$1"
        printf ' "expect=recovery image" "expect=# "'
        while IFS= read -r step; do
            printf " '%s'" "$step"
        done < "$4"
        printf '\n'
    } > "$1.sh"
    sh "$1.sh"
}
# The save: each range read off the disk into a file of its own on the save
# stick, by the machine's loop over the list. The input block is the largest
# the range's alignment allows, since dd is a system call per block and a
# range of a gibibyte in 512-byte blocks is hours on the machine as well as
# in the emulator; the ranges the grow writes are aligned to eight sectors.
# The output block is a mebibyte whatever the input's, because the file
# system syncs and flushes the disk at every write call, and a 4 KiB write
# per call would cost a flush per page of the save.
{
    # shellcheck disable=SC2016
    echo 'send=v=$(grep ^identity /proc/disk | grep " fixed " | sed -n 2p | cut -d" " -f2); mkdir -p /save && mount -t exfat /dev/disk${v}p1 /save && echo SAVE""MOUNTED'
    echo 'expect=SAVEMOUNTED'
    # shellcheck disable=SC2016
    echo 'send=d=$(grep ^identity /proc/disk | grep " fixed " | head -1 | cut -d" " -f2); i=0; while read s c; do u=1; for t in 2048 8; do if [ $((s % t)) = 0 ] && [ $((c % t)) = 0 ] && [ $u = 1 ]; then u=$t; fi; done; dd if=/dev/disk$d of=/save/range-$i ibs=$((u * 512)) obs=1M skip=$((s / u)) count=$((c / u)) 2>/dev/null || echo SAVEFAIL $i; i=$((i + 1)); done < /save/ranges; sync; echo SAVED""ALL $i'
    echo "expect=SAVEDALL $NRANGES"
    echo 'send=umount /save && echo SAVE""DONE'
    echo 'expect=SAVEDONE'
} > "$R/save.steps"
T0=$(date +%s)
if rehearse_boot "$R/save.log" 21600 SAVEDONE "$R/save.steps" > "$R/save.harness" 2>&1 &&
    ! grep -aq "^SAVEFAIL " "$R/save.log"; then
    SAVE_S=$(( $(date +%s) - T0 ))
    note "the save of $NRANGES ranges took $SAVE_S s"
else
    sed -n 's/^HARNESS: /rehearse-sitting: /p' "$R/save.harness" >&2
    bad "the save"
fi
# The install, through the product stick, the root grown to the disk's end.
{
    echo 'send=rach2-install'
    echo 'expect=disk shall Rach2 be written to'; echo 'send=/dev/disk0'
    echo 'expect=the password for root'; echo "send=$NEWPASS"
    echo 'expect=again'; echo "send=$NEWPASS"
    echo 'expect=The name of the first account'; echo "send=$USERNAME"
    echo "expect=the password for $USERNAME"; echo "send=$USERPASS"
    echo 'expect=again'; echo "send=$USERPASS"
    echo 'expect=uthorized keys'; echo 'send='
    echo 'expect=key for the recovery system'; echo 'send='
    echo 'expect=row the root'; echo 'send='
    echo 'expect=to go on, or stop to stop'; echo 'send=/dev/disk0'
    echo 'expect=the image on the stick is the one the manifest describes'
    echo 'expect=what reached the disk is the image'
    echo 'expect=the table is the one the image carries'
    echo "expect=the accounts are root and $USERNAME"
    echo 'expect=growing the root into the rest of the disk'
    echo 'expect=installed:'; echo 'send=echo INSTALL""DONE'
    echo 'expect=INSTALLDONE'
} > "$R/install.steps"
T0=$(date +%s)
if rehearse_boot "$R/install.log" 21600 INSTALLDONE "$R/install.steps" > "$R/install.harness" 2>&1; then
    INSTALL_S=$(( $(date +%s) - T0 ))
    note "the install and the grow took $INSTALL_S s in the emulator, against the sitting's budget of 600 s on the machine"
else
    sed -n 's/^HARNESS: /rehearse-sitting: /p' "$R/install.harness" >&2
    bad "the install"
fi
# Every sector the install wrote lies in the save set: growset judges its
# own ranges from the overlay's map, and an extent it calls outside must lie
# inside the ranges saved, the image's extent and the disk's end among them.
# covered RANGES CHECK: status 1 when an outside extent of CHECK has a sector
# that no range of RANGES, which are in ascending order, covers.
covered() {
    awk 'NR == FNR { s[NR] = $1; e[NR] = $1 + $2; n = NR; next }
        $1 == "outside" { x = $2; for (i = 1; i <= n; i++) if (s[i] <= x && x < e[i]) x = e[i]
            if (x < $2 + $3) bad = 1 }
        END { exit bad }' "$1" "$2"
}
qemu-img map --output=json "$R/disk.qcow2" > "$R/after-install.json" || bad "the overlay's map"
grep -q '"start"' "$R/after-install.json" || bad "the overlay's map lists no extent, and the install wrote"
growset_rc=0
"$GROWSET" --lba "$ROOT_START" --map "$R/after-install.json" "$R/head.vol" $((SECTORS - 33 - ROOT_START)) > "$R/after-install.check" 2>&1 || growset_rc=$?
[ "$growset_rc" -eq 0 ] || grep -q '^outside' "$R/after-install.check" || bad "growset failed on the install's map: $(head -1 "$R/after-install.check")"
if covered "$R/ranges" "$R/after-install.check"; then
    note "every sector the install and the grow wrote lies in the save set: OK"
else
    grep '^outside' "$R/after-install.check" | head -5 >&2
    bad "the install wrote outside the save set"
fi
# The restore: each range put back with reflash from the save stick by the
# machine's loop, then each read back with `reflash --verify` into a file
# of digests the host compares with its own digests of the saved bytes,
# which are zeros of each length.
{
    # shellcheck disable=SC2016
    echo 'send=v=$(grep ^identity /proc/disk | grep " fixed " | sed -n 2p | cut -d" " -f2); mkdir -p /save && mount -t exfat /dev/disk${v}p1 /save && echo SAVE""MOUNTED'
    echo 'expect=SAVEMOUNTED'
    # shellcheck disable=SC2016
    echo 'send=d=$(grep ^identity /proc/disk | grep " fixed " | head -1 | cut -d" " -f2); i=0; while read s c; do reflash /dev/disk$d $((s * 512)) $((c * 512)) < /save/range-$i || echo PUTFAIL $i; i=$((i + 1)); done < /save/ranges; echo PUT""ALL $i'
    echo "expect=PUTALL $NRANGES"
    # shellcheck disable=SC2016
    echo 'send=: > /save/verify; while read s c; do reflash --verify /dev/disk$d $((s * 512)) $((c * 512)) >> /save/verify; done < /save/ranges; sync; echo VERIFY""ALL'
    echo 'expect=VERIFYALL'
    echo 'send=umount /save && echo RESTORE""DONE'
    echo 'expect=RESTOREDONE'
} > "$R/restore.steps"
T0=$(date +%s)
if rehearse_boot "$R/restore.log" 21600 RESTOREDONE "$R/restore.steps" > "$R/restore.harness" 2>&1 &&
    ! grep -aq "^PUTFAIL " "$R/restore.log"; then
    RESTORE_S=$(( $(date +%s) - T0 ))
    note "the restore of $NRANGES ranges and their readback took $RESTORE_S s"
else
    sed -n 's/^HARNESS: /rehearse-sitting: /p' "$R/restore.harness" >&2
    bad "the restore"
fi
# What the machine read back against what was saved: each range's mark and
# then zeros to its length.
"$repo/build/tests/host/mkexfatfixture" --check "$R/save-stick.img" 2048 --read /verify > "$R/verify.machine" 2>/dev/null || bad "the digests off the save disk"
: > "$R/verify.host"
while read -r start count; do
    mark="range $start $count"
    { printf '%s\n' "$mark"; head -c $((count * 512 - ${#mark} - 1)) /dev/zero; } |
        shasum -a 256 | cut -d' ' -f1 >> "$R/verify.host"
done < "$R/ranges"
if [ "$(tr -d '\r' < "$R/verify.machine" | awk '{print $3}')" = "$(cat "$R/verify.host")" ]; then
    note "every range read back as the bytes that were saved: OK"
else
    bad "the ranges read back differ from what was saved"
fi
qemu-img map --output=json "$R/disk.qcow2" > "$R/after-restore.json" || bad "the overlay's map after the restore"
growset_rc=0
"$GROWSET" --lba "$ROOT_START" --map "$R/after-restore.json" "$R/head.vol" $((SECTORS - 33 - ROOT_START)) > "$R/after-restore.check" 2>&1 || growset_rc=$?
[ "$growset_rc" -eq 0 ] || grep -q '^outside' "$R/after-restore.check" || bad "growset failed on the restore's map: $(head -1 "$R/after-restore.check")"
if covered "$R/ranges" "$R/after-restore.check"; then
    note "the restore wrote nothing outside the save set either: OK"
else
    bad "the restore wrote outside the save set"
fi
