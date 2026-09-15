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
"$mkexfat" --make "$work/store.vol" --sectors 262144 --offset 2048 \
    --add "seed.ext2=$work/seed.ext2" --add "grow.plan=$work/plan/grow.plan" \
    --add "ranges.txt=$work/plan/ranges.txt" \
    --add "programs.sha256=$work/plan/programs.sha256" \
    --add "machine_grow.sh=$work/plan/machine_grow.sh" > "$work/store.make.log"
dd if=/dev/zero of="$work/store.img" bs=1048576 count=1 2>"$work/store-prefix.log"
cat "$work/store.vol" >> "$work/store.img"
"$mkmbr" --lba "$work/store.img" 2048:262144:07 > "$work/store-mbr.log"
qemu-img create -q -f qcow2 -o cluster_size=4096 -F raw \
    -b "$work/base.raw" "$work/disk.qcow2"

boot()
{
    current_log="$work/$1.log"
    shift
    "$harness" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --drive "if=none,id=stick,format=raw,file=$work/stick.img" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
        --drive "if=none,id=target,format=qcow2,file=$work/disk.qcow2" \
        --qemu-arg=-device --qemu-arg=ide-hd,drive=target,bus=ide.0,unit=0,serial=GROWTEST \
        --qemu-arg=-drive --qemu-arg="file=$work/store.img,format=raw,if=ide,index=1,media=disk" \
        --timeout 900 --steptmo 300 --ok GROW_MACHINE_DONE --log "$current_log" \
        'expect=recovery image' 'expect=# ' \
        'send=mkdir -p /mnt/grow /tmp/grow-plan && mount -t exfat /dev/disk1p1 /mnt/grow && cp /mnt/grow/grow.plan /mnt/grow/ranges.txt /mnt/grow/programs.sha256 /mnt/grow/machine_grow.sh /tmp/grow-plan/ && echo GROW_""SETUP' \
        'expect=GROW_SETUP' "$@" || fail 'the recovery boot'
}

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
