#!/bin/sh
# grow_machine.sh HARNESS ISO INSTALLER_DISK SEALED DIRECTORY [space |
# space-mutate]: rehearse the grow procedure of tools/dev/machine_grow.sh in
# QEMU on a 64 MiB root, booted from the recovery stick, with the backups on
# an exFAT store.
#
# The target is the first 130 MiB of SEALED, a disk of the machine's layout,
# its boot environment set to start recovery, and a 64 MiB root, under a
# qcow2 overlay. Its table is an MBR, or with RACH2_GROW_TABLE=gpt a GUID
# table, whose backup takes 33 sectors after the root; SEALED must have the
# same kind. `save` must back
# up the ranges and the boot partition without changing the target, and a
# `grow` before the host has checked the backup must be refused. The host
# then reads and checks the backup and writes host.sha256; `grow` must grow
# an 8 MiB seed to 64 MiB, `restore` must bring back every byte but the boot
# environment's, and every write must be in the saved sectors. With
# RACH2_GROW_SMALL=1 a bad plan and a corrupt boot copy must also be refused
# with no target byte written; then each of save, grow, restore and the
# repair of the boot environment is cut at a completed transfer, and a retry
# must restore every byte and the target must boot without the stick; and
# machine_grow_full_test.sh and machine_grow_space.sh run, on an MBR only.
# `space` and `space-mutate` give the prepared directory to
# machine_grow_space.sh.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
harness=$1
iso=$2
installer_disk=$3
sealed=$4
directory=$5
work=$(mktemp -d "$directory/machine.XXXXXX")
work=$(CDPATH='' cd -- "$work" && pwd)
echo "grow-machine-test: logs: $work"
current_log=
started=$(date +%s)
small=${RACH2_GROW_SMALL:-0}
table=${RACH2_GROW_TABLE:-mbr}
case "$table" in
    mbr) sectors=397312; disk_id=1acef00d ;;
    gpt) sectors=397345; disk_id=1acef00d-0f1e-4d2c-8b3a-c0ffee000001 ;;
    *) echo "grow-machine-test: RACH2_GROW_TABLE is mbr or gpt, not $table" >&2; exit 1 ;;
esac
# Report $*, print the current boot's log, and exit 1.
fail()
{
    echo "grow-machine-test: failed: $*" >&2
    [ -z "$current_log" ] || cat "$current_log" >&2
    exit 1
}
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
mkexfat="$repo/build/tests/host/mkexfatfixture"
mkmbr="$repo/build/tools/image/mkmbr"
sh tools/image/mkinstallstick.sh "$iso" "$installer_disk" "$work/stick.img" \
    "$mkexfat" "$mkmbr" "$repo/build/tools/image/stickcheck" '' 32 \
    > "$work/stick.make.log" 2>&1 || fail 'the recovery stick with extra space'
dd if="$sealed" of="$work/base.raw" bs=1048576 count=130 2>"$work/prefix.log"
[ "$(wc -c < "$work/base.raw" | tr -d ' ')" = 136314880 ] || fail 'the boot and recovery prefix'
# The fixture boots recovery from its own disk: the boot partition's GRUB
# environment saves the entry recovery, and the root has owner data and no
# installed userland.
dd if="$work/base.raw" of="$work/boot.vol" bs=1048576 skip=1 count=64 2>"$work/boot-copy.log"
{ printf '# GRUB Environment Block\nsaved_entry=recovery\n'; } > "$work/grubenv"
env_length=$(wc -c < "$work/grubenv" | tr -d ' ')
awk -v n="$((1024 - env_length))" 'BEGIN { for (i = 0; i < n; i++) printf "#" }' >> "$work/grubenv"
mcopy -o -i "$work/boot.vol" "$work/grubenv" ::/boot/grub/grubenv
# Only fixture files, never a host disk node.
dd if="$work/boot.vol" of="$work/base.raw" bs=1048576 seek=1 count=64 conv=notrunc 2>>"$work/boot-copy.log"
perl -e 'print chr(0xa5) x 1048576 for 1..64' > "$work/original.ext2"
dd if=/dev/zero of="$work/seed.ext2" bs=1048576 count=8 2>"$work/seed.log"
mkdir "$work/original-files" "$work/seed-files"
printf 'preserve the installed root\n' > "$work/original-files/owner.txt"
printf 'temporary grow seed\n' > "$work/seed-files/seed.txt"
for kind in original seed; do
    E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 1024 -I 128 \
        -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
        -d "$work/$kind-files" "$work/$kind.ext2" >"$work/$kind.mkfs.log" 2>&1 \
        || fail "formatting $kind"
done
cat "$work/original.ext2" >> "$work/base.raw"
if [ "$table" = mbr ]; then
    "$mkmbr" --lba --id "$disk_id" "$work/base.raw" \
        2048:131072:0c:boot 133120:131072:0c 266240:131072:83 > "$work/mbr.log"
else
    # The entries of tools/image/mkdisk_grub.sh, with the root filling the
    # disk as on a grown machine. mkgpt writes zeros for the boot code of
    # the first sector, so GRUB's is put back.
    dd if=/dev/zero bs=512 count=33 >> "$work/base.raw" 2>"$work/backup-space.log"
    dd if="$work/base.raw" of="$work/boot-code" bs=440 count=1 2>"$work/boot-code.log"
    "$repo/build/tools/image/mkgpt" --guid "$disk_id" "$work/base.raw" \
        "2048:131072:fat=Rach2 boot" "133120:131072:data=Rach2 recovery" \
        "266240:131072:linux=Rach2 root" "34:2014:bios=BIOS boot" > "$work/gpt.log"
    dd if="$work/boot-code" of="$work/base.raw" bs=440 count=1 conv=notrunc 2>>"$work/boot-code.log"
fi
[ "$(wc -c < "$work/base.raw" | tr -d ' ')" = "$((sectors * 512))" ] || fail 'the target size'
sh tools/dev/machine_grow_plan.sh "$work/seed.ext2" "$sectors" GROWTEST "$disk_id" "$work/plan"
tar -C "$work/plan" -cf "$work/grow-tools.tar" grow.plan ranges.txt machine_grow.sh programs.sha256 seed.sha256
# machine-cut.sh is a copy of the script with cut points: with GROW_TEST_CUT
# set to save, grow, restore or repair, the guest stops after a completed
# transfer while the action still has work to do. The faults are only in
# this copy.
awk '
    /^check_plan$/ {
        print "cut() {"
        print "    [ \"${GROW_TEST_CUT:-}\" = \"$1\" ] || return 0"
        print "    sync; echo machine-grow-test: cut: $1 >&2"
        print "    echo GROW_MACHINE_DONE >&2"
        print "    while :; do sleep 1; done"
        print "}"
    }
    /reflash -p/ {
        print "    if [ \"${GROW_TEST_CUT:-}\" = grow ]; then"
        print "        dd if=\"$store/seed.ext2\" bs=1048576 count=1 | reflash \"$disk\" \"$((start * 512))\" 1048576 || fail prefix"
        print "        cut grow"
        print "    fi"
    }
    /rm -f "\$work\/boot-write.status"/ {
        print "            if [ \"${GROW_TEST_CUT:-}\" = repair ] && [ \"$part\" = environment ]; then"
        print "                dd if=\"$work/recovery.env\" of=\"$work/env-prefix\" bs=512 count=1"
        print "                reflash \"$disk\" \"$((1048576 + at))\" 512 < \"$work/env-prefix\" || fail prefix"
        print "                cut repair"
        print "            fi"
    }
    {print}
    /2>>"\$work\/reads.log"/ {print "        if [ \"$action\" = save ] && [ \"$row\" = 2 ]; then cut save; fi"}
    /offset=\$\(\(offset \+ amount\)\)/ {print "        if [ \"$row\" = 2 ]; then cut restore; fi"}
' "$work/plan/machine_grow.sh" > "$work/plan/machine-cut.sh"
sh -n "$work/plan/machine-cut.sh"
for point in 'cut save' 'cut grow' 'cut restore' 'cut repair'; do
    grep -q "$point" "$work/plan/machine-cut.sh" || fail "missing $point injection"
done
# bad-ranges.txt moves the last range of the grow, before a GUID table's
# backup, by its own length.
awk -v table="$table" 'NR == FNR {n++; next} FNR == n - (table == "gpt") {$1 += $2} {print}' \
    "$work/plan/ranges.txt" "$work/plan/ranges.txt" > "$work/plan/bad-ranges.txt"
# corrupt-boot.sh changes the boot copy's last byte before its checksum is
# made.
awk '/    sync$/ && !done {print "    printf x | dd of=\"$saved/boot.img\" bs=1 seek=67108863 conv=notrunc"; done=1} {print}' \
    "$work/plan/machine_grow.sh" > "$work/plan/corrupt-boot.sh"
tar -C "$work/plan" -cf "$work/grow-tests.tar" machine-cut.sh bad-ranges.txt corrupt-boot.sh
"$mkexfat" --make "$work/store.vol" --sectors 262144 --offset 2048 \
    --add "seed.ext2=$work/seed.ext2" --add "grow-tools.tar=$work/grow-tools.tar" \
    --add "grow-tests.tar=$work/grow-tests.tar" > "$work/store.make.log"
dd if=/dev/zero of="$work/store.img" bs=1048576 count=1 2>"$work/store-prefix.log"
cat "$work/store.vol" >> "$work/store.img"
"$mkmbr" --lba "$work/store.img" 2048:262144:07 > "$work/store-mbr.log"
cp "$work/store.img" "$work/store.initial.img"
if [ "${6:-}" = space ] || [ "${6:-}" = space-mutate ]; then
    exec sh tests/system/machine_grow_space.sh "$harness" "$work" "$6"
fi
qemu-img create -q -f qcow2 -o cluster_size=4096 -F raw \
    -b "$work/base.raw" "$work/disk.qcow2"

# boot NAME STEPS...: boot the recovery stick with the target and the store
# attached, unpack the plan from the store, and run STEPS. A
# `machine-grow: failed:` line fails the boot, except in the save and
# rejected boots, whose steps expect failures.
boot()
{
    current_log="$work/$1.log"
    case "$1" in
        save|rejected) forbidden=GROW_UNEXPECTED_FAILURE ;;
        *) forbidden='machine-grow: failed:' ;;
    esac
    shift
    "$harness" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --drive "if=none,id=stick,format=raw,file=$work/stick.img" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
        --drive "if=none,id=target,format=qcow2,file=$work/disk.qcow2" \
        --qemu-arg=-device --qemu-arg=ide-hd,drive=target,bus=ide.0,unit=0,serial=GROWTEST \
        --qemu-arg=-drive --qemu-arg="file=$work/store.img,format=raw,if=ide,index=1,media=disk" \
        --timeout 900 --steptmo 300 --ok GROW_MACHINE_DONE --never "$forbidden" --log "$current_log" \
        'expect=recovery image' 'expect=# ' \
        'send=test ! -e /tmp/grow-plan && mkdir -p /mnt/grow /tmp/grow-plan && mount -t exfat /dev/disk1p1 /mnt/grow && tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar && tar -C /tmp/grow-plan -xf /mnt/grow/grow-tests.tar && echo GROW_""SETUP' \
        'expect=GROW_SETUP' "$@" || fail 'the recovery boot'
}

# The unchanged script must refuse the bad plan and the corrupt boot copy
# before it writes the target; the comparison after the boot covers both.
if [ "$small" = 1 ]; then
    boot rejected \
        'send=cp /tmp/grow-plan/ranges.txt /tmp/grow-plan/good-ranges.txt && cp /tmp/grow-plan/bad-ranges.txt /tmp/grow-plan/ranges.txt && sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan; test "$?" = 1 && test ! -e /mnt/grow/saved.pending && echo BAD_RANGE_""REFUSED' \
        'expect=ranges differ from the seed and target geometry' 'expect=BAD_RANGE_REFUSED' \
        'send=cp /tmp/grow-plan/good-ranges.txt /tmp/grow-plan/ranges.txt && sh /tmp/grow-plan/corrupt-boot.sh save /dev/disk0 /mnt/grow /tmp/grow-plan; test "$?" = 1 && test ! -e /mnt/grow/saved && echo BAD_BOOT_""REFUSED' \
        'expect=the boot backup differs from the disk' 'expect=BAD_BOOT_REFUSED' \
        'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
    qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/after-rejected.raw"
    cmp "$work/base.raw" "$work/after-rejected.raw" || fail 'a rejected input changed the target'
    cp "$work/store.img" "$work/store.rejected.img"
    cp "$work/store.initial.img" "$work/store.img"
    echo 'grow-machine-test: bad ranges and a self-checksummed corrupt boot copy write no target byte: OK'
fi

# The status is expanded by the recovery shell.
# shellcheck disable=SC2016
boot save \
    'send=sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan && echo GROW_""SAVED' \
    'expect=GROW_SAVED' \
    'send=sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan; test "$?" = 1 && echo GROW_""REFUSED' \
    'expect=host verification is absent' 'expect=GROW_REFUSED' \
    'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/after-save.raw"
cmp "$work/base.raw" "$work/after-save.raw" || fail 'the save or refused grow changed the disk'
mkdir "$work/host-copy"
for name in backup.bin boot.img grow.plan ranges.txt SHA256SUMS boot.status; do
    "$mkexfat" --check "$work/store.img" 2048 --read "saved/$name" \
        > "$work/host-copy/$name" || fail "reading saved/$name on the host"
done
(cd "$work/host-copy" && shasum -a 256 -c SHA256SUMS) || fail 'the host backup checksums'
dd if="$work/base.raw" of="$work/original-boot.img" bs=1048576 skip=1 count=64 \
    2>"$work/boot-read.log"
cmp "$work/original-boot.img" "$work/host-copy/boot.img" || fail 'the boot backup'
echo 'grow-machine-test: the backup reaches the host and an unverified grow writes nothing: OK'

ack="send=printf '%s\\n'"
while read -r sum name; do
    case "$sum" in ''|*[!0-9a-f]*) fail 'invalid saved checksum' ;; esac
    [ "${#sum}" = 64 ] || fail 'short saved checksum'
    case "$name" in backup.bin|boot.img|grow.plan|ranges.txt) ;; *) fail 'unexpected saved file' ;; esac
    ack="$ack '$sum  $name'"
done < "$work/host-copy/SHA256SUMS"
ack="$ack > /mnt/grow/saved/host.sha256"
boot grow "$ack" \
    'send=sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan && echo GROW_""FINISHED' \
    'expect=the measured grow and read-only check passed' 'expect=GROW_FINISHED' \
    'send=dd if=/dev/zero of=/tmp/broken-mbr bs=512 count=1 && reflash /dev/disk0 0 512 < /tmp/broken-mbr && diskctl reread /dev/disk0 && echo GROW_TABLE_""BROKEN' \
    'expect=GROW_TABLE_BROKEN' \
    'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
grep -q 'growroot: entry: /dev/disk0p3 from 16384 to 131072 sectors' "$current_log" \
    || fail 'the temporary table did not select the small seed'
grep -q 'growroot: measure: resize ' "$current_log" || fail 'the resize measurement is absent'
echo 'grow-machine-test: the temporary seed grows from 8 to 64 MiB with recovery saved: OK'

# check_writes NAME: require every extent the overlay has written to be in a
# saved range of the plan or in the boot partition, sectors 2048 to 133119,
# whose environment the grow writes beside the seed.
check_writes()
{
    qemu-img map --output=json "$work/disk.qcow2" > "$work/$1.map"
    map_status=0
    "$repo/build/tools/inspect/growset" --lba 266240 --map "$work/$1.map" \
        "$work/seed.ext2" 131072 > "$work/$1.cover" || map_status=$?
    [ "$map_status" = 0 ] || grep -q '^outside ' "$work/$1.cover" \
        || fail 'reading the written extents'
    grep -Eq '^coverage: [1-9][0-9]* extents written,' "$work/$1.cover" \
        || fail 'no written extent was checked'
    awk '
        NR == FNR { first[++n] = $1; end[n] = $1 + $2; next }
        $1 == "outside" {
            covered = ($2 >= 2048 && $2 + $3 <= 133120)
            for (i = 1; i <= n; i++)
                if ($2 >= first[i] && $2 + $3 <= end[i]) covered = 1
            if (!covered) { print; bad = 1 }
        }
        END { exit bad }
    ' "$work/plan/ranges.txt" "$work/$1.cover" || fail 'a write outside the saved sectors'
}
check_writes grown
if [ "$small" = 1 ]; then
    cp "$work/store.img" "$work/store.saved.img"
    qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/grown.raw"
fi

boot restore \
    'send=sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan && echo GROW_""RESTORED' \
    'expect=every saved range matches and the restored root checks clean' 'expect=GROW_RESTORED' \
    'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
check_writes restored
qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/restored.raw"
# The environment is one cluster on an MBR disk's boot partition and two of
# one sector on a GUID table's, which growboot has found consecutive.
env_sector=$(awk '$2 == "grubenv:" && ($9 == 1 || ($9 == 2 && $6 == 2)) && $11 == 1024 {
    gsub(/,/, "", $5); print $5 }' "$work/host-copy/boot.status")
case "$env_sector" in ''|*[!0-9]*) fail 'the fixture environment is not in two consecutive sectors' ;; esac
env_sector=$((2048 + env_sector))
cp "$work/restored.raw" "$work/normalized.raw"
dd if="$work/base.raw" of="$work/normalized.raw" bs=512 skip="$env_sector" \
    seek="$env_sector" count=2 conv=notrunc 2>"$work/env-normalize.log"
cmp "$work/base.raw" "$work/normalized.raw" || fail 'a byte outside the boot environment was not restored'
echo 'grow-machine-test: result: a missing table and the complete root are restored; recovery remains saved'

# disk_boot NAME: boot the target with no stick attached, so that GRUB, the
# recovery kernel and its RAM image all come from it; recovery must be the
# saved entry, and the restored root must have the owner file.
disk_boot()
{
    current_log="$work/$1-boot.log"
    "$harness" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
        --drive "file=$work/disk.qcow2,format=qcow2,if=ide,index=0,media=disk" \
        --timeout 90 --steptmo 60 --ok GROW_DISK_BOOTS --log "$current_log" \
        'expect=recovery image' 'expect=# ' \
        'send=cat /proc/cmdline; bootctl --target /dev/disk0p1 status' \
        'expect=rach2.slot=recovery' 'expect=bootctl: saved_entry: recovery' \
        'expect=bootctl: next_entry: absent' \
        'send=mkdir -p /mnt/root && mount -t ext2 -o ro /dev/disk0p3 /mnt/root && cat /mnt/root/owner.txt && umount /mnt/root && echo GROW_DISK_""BOOTS' \
        'expect=preserve the installed root' 'expect=GROW_DISK_BOOTS' || fail 'booting the target without USB'
}

[ "$small" = 1 ] || exit 0
disk_boot baseline
for point in save grow restore repair; do
    rm "$work/disk.qcow2"
    case "$point" in
        save) backing=base; cp "$work/store.initial.img" "$work/store.img" ;;
        grow) backing=base; cp "$work/store.saved.img" "$work/store.img" ;;
        *) backing=grown; cp "$work/store.saved.img" "$work/store.img" ;;
    esac
    qemu-img create -q -f qcow2 -o cluster_size=4096 -F raw \
        -b "$work/$backing.raw" "$work/disk.qcow2"
    if [ "$point" = repair ]; then
        # The grown target's MBR is zero; it is written back first, so that
        # bootctl reads the zeroed environment and reports its header.
        boot "$point-cut" \
            "send=dd if=/mnt/grow/saved/backup.bin of=/tmp/mbr bs=512 count=1 && reflash /dev/disk0 0 512 < /tmp/mbr && diskctl reread /dev/disk0 && dd if=/dev/zero of=/tmp/bad-env bs=512 count=2 && reflash /dev/disk0 $((env_sector * 512)) 1024 < /tmp/bad-env && echo ENV_\"\"DAMAGED" \
            'expect=ENV_DAMAGED' \
            'send=bootctl --target /dev/disk0p1 env saved_entry recovery; test "$?" = 1 && echo ENV_""UNPARSEABLE' \
            "expect=the environment block header is not GRUB.s" 'expect=ENV_UNPARSEABLE' \
            'send=GROW_TEST_CUT=repair sh /tmp/grow-plan/machine-cut.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan' \
            'expect=machine-grow-test: cut: repair' 'expect=GROW_MACHINE_DONE'
    else
        boot "$point-cut" \
            "send=GROW_TEST_CUT=$point sh /tmp/grow-plan/machine-cut.sh $point /dev/disk0 /mnt/grow /tmp/grow-plan" \
            "expect=machine-grow-test: cut: $point" 'expect=GROW_MACHINE_DONE'
    fi
    qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/$point-cut.raw"
    cp "$work/store.img" "$work/store.$point-cut.img"
    if [ "$point" = save ]; then
        cmp "$work/base.raw" "$work/$point-cut.raw" || fail 'an interrupted save changed the target'
        boot save-retry \
            'send=test ! -e /mnt/grow/saved && test -d /mnt/grow/saved.pending && mv /mnt/grow/saved.pending /mnt/grow/interrupted-save && sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan && echo SAVE_""RETRIED' \
            'expect=SAVE_RETRIED' \
            'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
        # The retried backup must be the same as the host copy checked before.
        mkdir "$work/retried-copy"
        for name in backup.bin boot.img grow.plan ranges.txt SHA256SUMS; do
            "$mkexfat" --check "$work/store.img" 2048 --read "saved/$name" > "$work/retried-copy/$name"
            cmp "$work/host-copy/$name" "$work/retried-copy/$name" || fail "retried $name differs"
        done
        (cd "$work/retried-copy" && shasum -a 256 -c SHA256SUMS) || fail 'retried host checksums'
    else
        cmp -s "$work/base.raw" "$work/$point-cut.raw" && fail "$point did not leave work to restore"
        boot "$point-retry" \
            'send=sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan && echo RESTORE_""RETRIED' \
            'expect=every saved range matches and the restored root checks clean' 'expect=RESTORE_RETRIED' \
            'send=umount /mnt/grow && echo GROW_MACHINE_""DONE'
        if [ "$point" = repair ]; then
            grep -q 'boot environment repaired from boot.img' "$current_log" || fail 'the torn environment repair was not replayed'
        else
            grep -q 'boot environment unchanged' "$current_log" || fail 'restoration rewrote recovery'
        fi
    fi
    if [ "$point" != save ]; then check_writes "$point-retried"; fi
    qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/$point-retried.raw"
    cmp "$work/base.raw" "$work/$point-retried.raw" || fail "$point did not restore every original byte"
    disk_boot "$point"
    qemu-img convert -f qcow2 -O raw "$work/disk.qcow2" "$work/$point-booted.raw"
    cmp "$work/base.raw" "$work/$point-booted.raw" || fail "$point boot changed the restored disk"
    echo "grow-machine-test: interrupted $point: original bytes restored and target boots without USB: OK"
done
echo "grow-machine-test: result: interruption matrix completed in $(($(date +%s) - started)) seconds"

# The replay of the Samsung's ranges, and machine_grow_space.sh, which runs
# every phase in one full RAM root, run only with RACH2_GROW_SMALL=1, and
# only on the MBR, since neither depends on the table.
[ "$table" = mbr ] || exit 0
sh tests/system/machine_grow_full_test.sh
sh tests/system/machine_grow_space.sh "$harness" "$work" space
