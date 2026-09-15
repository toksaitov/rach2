#!/bin/sh
# Rehearse the recovery procedure on a 64 MiB root, including both backups.
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
# The original fixture boots recovery from its own disk. Its ext2 root has
# owner data but no installed userland; boot checks must identify this slot.
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
"$mkmbr" --lba --id 1acef00d "$work/base.raw" \
    2048:131072:0c:boot 133120:131072:0c 266240:131072:83 > "$work/mbr.log"
sh tools/dev/machine_grow_plan.sh "$work/seed.ext2" 397312 GROWTEST 1acef00d "$work/plan"
tar -C "$work/plan" -cf "$work/grow-tools.tar" grow.plan ranges.txt machine_grow.sh programs.sha256 seed.sha256
# Faults exist only in this private helper. The VM is stopped at a completed
# transfer boundary while the enclosing operation still has work to do.
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
awk 'NR == FNR {n++; next} FNR == n {$1 += $2} {print}' \
    "$work/plan/ranges.txt" "$work/plan/ranges.txt" > "$work/plan/bad-ranges.txt"
# A same-length corruption before the initial checksum is made.
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
qemu-img create -q -f qcow2 -o cluster_size=4096 -F raw \
    -b "$work/base.raw" "$work/disk.qcow2"

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

# The complete guest helper must reject both bad inputs before writing the
# target. The byte comparison after the boot covers all rejected attempts.
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

# The seed and boot environment add writes to the grow tool's metadata set.
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
env_sector=$(awk '$2 == "grubenv:" && $9 == 1 && $11 == 1024 {gsub(/,/, "", $5); print $5}' \
    "$work/host-copy/boot.status")
case "$env_sector" in ''|*[!0-9]*) fail 'the fixture environment is not one contiguous cluster' ;; esac
env_sector=$((2048 + env_sector))
cp "$work/restored.raw" "$work/normalized.raw"
dd if="$work/base.raw" of="$work/normalized.raw" bs=512 skip="$env_sector" \
    seek="$env_sector" count=2 conv=notrunc 2>"$work/env-normalize.log"
cmp "$work/base.raw" "$work/normalized.raw" || fail 'a byte outside the boot environment was not restored'
echo 'grow-machine-test: result: a missing table and the complete root are restored; recovery remains saved'

# No stick is attached: GRUB, the recovery kernel and its RAM image must all
# boot from the repaired target. Mount and read the restored owner file too.
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
        # The grown fixture has a missing MBR. Restore that sector so bootctl
        # reaches the damaged header and its diagnostic proves the condition.
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
        # Extract and compare the retried backup with the previously verified
        # host copy. A new manifest is acknowledged only after those checks.
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
