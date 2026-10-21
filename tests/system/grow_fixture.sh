#!/bin/sh
#
# grow_fixture.sh: make the disks of the grow suite's machine cases,
# tests/cases/machine.grow.json, which the rules after CASE_IMAGES in
# tests/system/suites.mk run. Each is the base of an overlay or the source
# of a private copy, so a case never changes it.
#
#   base MKMBR OUT
#       A disk of 16 GiB, sparse, with a 1 GiB ext2 volume in entry 1 of an
#       MBR at sector 2048, of disk identifier 47470001, and nothing after
#       it: the root the measured grow grows.
#   full MKMBR BASE OUT
#       BASE with its entry already reaching the disk's end, as a grow
#       stopped between the entry and the resize leaves it.
#   target TABLE SEALED MKMBR MKGPT OUT
#       The installed machine's disk on a 64 MiB root: the first 130 MiB of
#       SEALED, a disk of the machine's layout whose table is TABLE, mbr or
#       gpt, its boot environment set to start recovery, and a root of owner
#       data and no userland, filling the disk as on a grown machine. A GUID
#       table has its backup in 33 sectors after the root.
#   store TABLE MKEXFATFIXTURE MKMBR OUT
#       The exFAT store the grow's backups go to, on an MBR disk of its own:
#       the 8 MiB seed and grow-tools.tar, the plan that
#       tools/dev/machine_grow_plan.sh makes for TARGET's geometry with the
#       recovery programs of the architecture RACH2_ARCH names, and
#       grow-tests.tar, the faults of the plan the manual runs of the grow
#       inject (tests/system/grow_machine.sh with RACH2_GROW_SMALL=1).
#
# Usage: grow_fixture.sh KIND ARGS..., from the root of a built tree. The
# file is written beside OUT and renamed to it once it is whole; the
# directory of its parts, OUT.PID.d, is removed on every exit.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
# Report $* and exit 1.
fail() { echo "grow-fixture: failed: $*" >&2; exit 1; }
[ "$#" -ge 3 ] || fail 'usage: grow_fixture.sh base|full|target|store ARGS... OUT'
kind=$1
shift
eval "out=\${$#}"
# shellcheck disable=SC2154 # out is set by the eval
parts=$out.$$.d
new=$out.$$.new
trap 'rm -rf "$parts" "$new"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
rm -rf "$parts"
mkdir -p "$parts"
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
features=sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize

# The geometry of the measured grow's disk.
grow_sectors=33554432
grow_start=2048
grow_size=2097152
grow_id=47470001

# The machine's identity in TABLE: the disk's sectors and identifier.
machine_geometry() {
    case "$1" in
        mbr) sectors=397312; disk_id=1acef00d ;;
        gpt) sectors=397345; disk_id=1acef00d-0f1e-4d2c-8b3a-c0ffee000001 ;;
        *) fail "the table is mbr or gpt, not $1" ;;
    esac
}

case "$kind" in
    base)
        mkmbr=$1
        dd if=/dev/zero of="$new" bs=512 count=1 seek=$((grow_sectors - 1)) 2>"$parts/dd.log"
        E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 32768 -I 128 -O "$features" \
            -E offset=$((grow_start * 512)) "$new" $((grow_size / 2048))M > "$parts/mkfs.log" 2>&1 ||
            fail "the volume; see $parts/mkfs.log"
        "$mkmbr" --lba --id "$grow_id" "$new" "$grow_start:$grow_size:83" > /dev/null
        ;;
    full)
        mkmbr=$1
        base=$2
        cp -c "$base" "$new" 2>/dev/null || cp "$base" "$new"
        "$mkmbr" --lba --id "$grow_id" "$new" "$grow_start:$((grow_sectors - grow_start)):83" > /dev/null
        ;;
    target)
        machine_geometry "$1"
        table=$1
        sealed=$2
        mkmbr=$3
        mkgpt=$4
        dd if="$sealed" of="$new" bs=1048576 count=130 2>"$parts/prefix.log"
        [ "$(wc -c < "$new" | tr -d ' ')" = 136314880 ] || fail 'the boot and recovery prefix'
        # The disk boots recovery: the boot partition's GRUB environment
        # saves the entry recovery.
        dd if="$new" of="$parts/boot.vol" bs=1048576 skip=1 count=64 2>"$parts/boot-copy.log"
        printf '# GRUB Environment Block\nsaved_entry=recovery\n' > "$parts/grubenv"
        env_length=$(wc -c < "$parts/grubenv" | tr -d ' ')
        awk -v n="$((1024 - env_length))" 'BEGIN { for (i = 0; i < n; i++) printf "#" }' \
            >> "$parts/grubenv"
        mcopy -o -i "$parts/boot.vol" "$parts/grubenv" ::/boot/grub/grubenv
        dd if="$parts/boot.vol" of="$new" bs=1048576 seek=1 count=64 conv=notrunc \
            2>>"$parts/boot-copy.log"
        perl -e 'print chr(0xa5) x 1048576 for 1..64' > "$parts/original.ext2"
        mkdir "$parts/files"
        printf 'preserve the installed root\n' > "$parts/files/owner.txt"
        E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 1024 -I 128 -O "$features" \
            -d "$parts/files" "$parts/original.ext2" > "$parts/mkfs.log" 2>&1 ||
            fail "formatting the root; see $parts/mkfs.log"
        cat "$parts/original.ext2" >> "$new"
        if [ "$table" = mbr ]; then
            "$mkmbr" --lba --id "$disk_id" "$new" \
                2048:131072:0c:boot 133120:131072:0c 266240:131072:83 > "$parts/mbr.log"
        else
            # The entries of tools/image/mkdisk_grub.sh, with the root filling
            # the disk as on a grown machine. mkgpt writes zeros for the boot
            # code of the first sector, so GRUB's is put back.
            dd if=/dev/zero bs=512 count=33 >> "$new" 2>"$parts/backup-space.log"
            dd if="$new" of="$parts/boot-code" bs=440 count=1 2>"$parts/boot-code.log"
            "$mkgpt" --guid "$disk_id" "$new" \
                "2048:131072:fat=Rach2 boot" "133120:131072:data=Rach2 recovery" \
                "266240:131072:linux=Rach2 root" "34:2014:bios=BIOS boot" > "$parts/gpt.log"
            dd if="$parts/boot-code" of="$new" bs=440 count=1 conv=notrunc 2>>"$parts/boot-code.log"
        fi
        [ "$(wc -c < "$new" | tr -d ' ')" = "$((sectors * 512))" ] || fail 'the target size'
        ;;
    store)
        machine_geometry "$1"
        table=$1
        mkexfat=$2
        mkmbr=$3
        dd if=/dev/zero of="$parts/seed.ext2" bs=1048576 count=8 2>"$parts/seed.log"
        mkdir "$parts/files"
        printf 'temporary grow seed\n' > "$parts/files/seed.txt"
        E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 1024 -I 128 -O "$features" \
            -d "$parts/files" "$parts/seed.ext2" > "$parts/mkfs.log" 2>&1 ||
            fail "formatting the seed; see $parts/mkfs.log"
        sh "$repo/tools/dev/machine_grow_plan.sh" "$parts/seed.ext2" "$sectors" GROWTEST "$disk_id" \
            "$parts/plan" > "$parts/plan.log"
        tar -C "$parts/plan" -cf "$parts/grow-tools.tar" grow.plan ranges.txt machine_grow.sh \
            programs.sha256 seed.sha256
        # machine-cut.sh is a copy of the script with cut points: with
        # GROW_TEST_CUT set to save, grow, restore or repair, the guest stops
        # after a completed transfer while the action still has work to do.
        # The faults are only in this copy.
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
        ' "$parts/plan/machine_grow.sh" > "$parts/plan/machine-cut.sh"
        sh -n "$parts/plan/machine-cut.sh"
        for point in 'cut save' 'cut grow' 'cut restore' 'cut repair'; do
            grep -q "$point" "$parts/plan/machine-cut.sh" || fail "missing $point injection"
        done
        # bad-ranges.txt moves the last range of the grow, before a GUID
        # table's backup, by its own length.
        awk -v table="$table" 'NR == FNR {n++; next} FNR == n - (table == "gpt") {$1 += $2} {print}' \
            "$parts/plan/ranges.txt" "$parts/plan/ranges.txt" > "$parts/plan/bad-ranges.txt"
        # corrupt-boot.sh changes the boot copy's last byte before its
        # checksum is made.
        awk '/    sync$/ && !done {print "    printf x | dd of=\"$saved/boot.img\" bs=1 seek=67108863 conv=notrunc"; done=1} {print}' \
            "$parts/plan/machine_grow.sh" > "$parts/plan/corrupt-boot.sh"
        tar -C "$parts/plan" -cf "$parts/grow-tests.tar" machine-cut.sh bad-ranges.txt corrupt-boot.sh
        "$mkexfat" --make "$parts/store.vol" --sectors 262144 --offset 2048 \
            --add "seed.ext2=$parts/seed.ext2" --add "grow-tools.tar=$parts/grow-tools.tar" \
            --add "grow-tests.tar=$parts/grow-tests.tar" > "$parts/store.make.log"
        dd if=/dev/zero of="$new" bs=1048576 count=1 2>"$parts/store-prefix.log"
        cat "$parts/store.vol" >> "$new"
        "$mkmbr" --lba "$new" 2048:262144:07 > "$parts/store-mbr.log"
        ;;
    *)
        fail "no kind $kind"
        ;;
esac
mv "$new" "$out"
