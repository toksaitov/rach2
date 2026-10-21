#!/bin/sh
# parts_disks.sh: make the disks of the partition cases,
# tests/cases/machine.parts.json.
#
# Usage: parts_disks.sh arch DIR EXT2IMAGE
#        parts_disks.sh shared DIR
#
# `arch` makes the disks that hold the architecture's programs, in DIR:
#
#   a.disk  64 MiB, an MBR with the disk id 0a0a0a0a: a KRFFS partition of
#           disk-test.manifest at sector 2048, a FAT32 primary, an extended
#           partition holding three logical partitions (the second of them
#           FAT32), and a fourth primary;
#   b.disk  32 MiB, a GUID table of six labelled entries made with seed 1,
#           a FAT32 volume labelled DATA3 in entry 3 and EXT2IMAGE, an ext2
#           root, in entry 4;
#   b.gpt   what mkgpt said of b.disk, which gives each entry's GUID: the
#           cases name the root by entry 4's.
#
# `shared` makes the disks of host-made data alone, in DIR. c.disk is the
# table of b.disk with the same seed and no volumes, its primary header's
# checksum broken, so that a reader takes the table from the backup: the
# partition cases' clone of b.disk, and a disk of the recovery shell's
# `disk write`.
#
# `disk write` in the recovery kernel's debug shell checks the whole-disk
# write path by writing the boot disk's last sector and reading it back.
# Past every MBR partition that sector is scratch space, but on a
# GUID-partitioned disk it is the backup header, and a disk whose backup
# header is gone opens only in a repair tool. The command reads LBA 1 first
# and refuses a disk that has `EFI PART` there, and where LBA 1 has none it
# reads the last sector and refuses a disk that has it there; on an MBR disk
# it refuses a last sector that one of the four entries holds. Each disk
# below is 32 MiB, and each GUID table is b.disk's:
#
#   gpt-nombr.disk            sector 0 zeroed, so no protective MBR;
#   gpt-nombr-noprimary.disk  the same with the primary header zeroed as
#                             well, so that only the last sector says GPT;
#   gpt.disk                  both headers whole;
#   c.disk                    the primary header's checksum broken;
#   gpt-noprimary.disk        the primary header's sector zeroed, so that
#                             LBA 1 says nothing and the table is in the
#                             backup alone;
#   mbr-full.disk             an MBR whose one partition reaches the last
#                             sector, as a root grown into the whole disk
#                             does;
#   mbr-scratch.disk          an MBR whose one partition ends before the
#                             last sector, which the command must write.
#
# A protective MBR's entry covers the whole disk, so on gpt.disk, c.disk and
# gpt-noprimary.disk the MBR check refuses too, with another answer; on the
# two without one the GUID checks alone keep the disk.
#
# Each file is written under a temporary name and renamed once all of them
# are made.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MKMBR="$REPO/build/tools/image/mkmbr"
MKGPT="$REPO/build/tools/image/mkgpt"

usage() {
    echo 'usage: parts_disks.sh arch DIR EXT2IMAGE | shared DIR' >&2
    exit 2
}
[ "$#" -ge 2 ] || usage
MODE=$1
DIR=$2
case $MODE in
    arch) [ "$#" -eq 3 ] || usage; EXT2=$3 ;;
    shared) [ "$#" -eq 2 ] || usage ;;
    *) usage ;;
esac

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# fail REASON prints REASON and exits 1.
fail() { echo "parts-disks: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR" || fail "mkdir $DIR"
work=$(mktemp -d "$DIR/rach2-parts.XXXXXX") || fail "mktemp"

# zeros DISK MIB makes DISK a file of MIB MiB of zeros.
zeros() {
    dd if=/dev/zero of="$1" bs=1048576 count="$2" 2>/dev/null
}

# gpt_disk DISK [--damage-primary] makes DISK the 32 MiB GUID table of seed
# 1, with mkgpt's report on standard output.
gpt_disk() {
    disk=$1
    shift
    zeros "$disk" 32 || return 1
    "$MKGPT" --seed 1 "$@" "$disk" 64:2048:fat=ONE 4096:4096:data=TWO 8192:8192:fat=DATA3 \
        16384:32768:data=ROOT 49152:1024:data 50176:1024:data=SIX
}

# zero_sectors DISK FIRST COUNT zeroes COUNT sectors of DISK from FIRST.
zero_sectors() {
    dd if=/dev/zero of="$1" bs=512 seek="$2" count="$3" conv=notrunc 2>/dev/null
}

# mbr_disk DISK COUNT makes DISK a 32 MiB disk whose one primary partition,
# type 0x83, starts at sector 2048 and is COUNT sectors long.
mbr_disk() {
    zeros "$1" 32 || return 1
    "$MKMBR" --lba "$1" "2048:$2:83" > /dev/null
}

if [ "$MODE" = arch ]; then
    # Disk A: mkdisk lays the KRFFS partition at LBA 2048 for 48 MiB; the
    # table is then rewritten around it with the id, the FAT32 primary, the
    # extended partition and its logical ones, and the fourth primary.
    sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$work/a.disk" 64 48 \
        > /dev/null 2>&1 || fail "mkdisk"
    "$MKMBR" --lba --id 0a0a0a0a "$work/a.disk" 2048:98304:7f 100352:8192:0c 108544:14336:05 \
        122880:4096:83 108545:4096:83:logical 112642:4096:0c:logical 116739:4096:83:logical \
        > /dev/null || fail "mkmbr a.disk"
    mformat -i "$work/a.disk"@@$((100352 * 512)) -F -T 8192 -v APRIM :: || fail "mformat a.disk p2"
    mformat -i "$work/a.disk"@@$((112642 * 512)) -F -T 4096 -v ALOG :: || fail "mformat a.disk p6"
    # Disk B: the GUID table, with the FAT32 volume in entry 3 and the ext2
    # root in 4.
    gpt_disk "$work/b.disk" > "$work/b.gpt" || fail "mkgpt b.disk"
    mformat -i "$work/b.disk"@@$((8192 * 512)) -F -T 8192 -v DATA3 :: || fail "mformat b.disk p3"
    dd if="$EXT2" of="$work/b.disk" bs=512 seek=16384 conv=notrunc 2>/dev/null ||
        fail "the ext2 root into b.disk"
    grep -q '^mkgpt: entry 4 guid ' "$work/b.gpt" || fail "entry 4's GUID in b.gpt"
    names="a.disk b.disk b.gpt"
else
    gpt_disk "$work/c.disk" --damage-primary > /dev/null || fail "mkgpt c.disk"
    gpt_disk "$work/gpt.disk" > /dev/null || fail "mkgpt gpt.disk"
    gpt_disk "$work/gpt-nombr.disk" > /dev/null || fail "mkgpt gpt-nombr.disk"
    zero_sectors "$work/gpt-nombr.disk" 0 1 || fail "the protective MBR of gpt-nombr.disk"
    gpt_disk "$work/gpt-nombr-noprimary.disk" > /dev/null || fail "mkgpt gpt-nombr-noprimary.disk"
    zero_sectors "$work/gpt-nombr-noprimary.disk" 0 2 ||
        fail "the protective MBR and the primary header of gpt-nombr-noprimary.disk"
    gpt_disk "$work/gpt-noprimary.disk" > /dev/null || fail "mkgpt gpt-noprimary.disk"
    zero_sectors "$work/gpt-noprimary.disk" 1 1 || fail "the primary header of gpt-noprimary.disk"
    mbr_disk "$work/mbr-full.disk" 63488 || fail "mbr-full.disk"
    mbr_disk "$work/mbr-scratch.disk" 61440 || fail "mbr-scratch.disk"
    names="c.disk gpt.disk gpt-nombr.disk gpt-nombr-noprimary.disk gpt-noprimary.disk"
    names="$names mbr-full.disk mbr-scratch.disk"
fi
for name in $names; do
    mv "$work/$name" "$DIR/$name" || fail "mv $name"
done
