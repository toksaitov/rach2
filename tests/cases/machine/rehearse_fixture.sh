#!/bin/sh
# rehearse_fixture.sh: the fixture of the cases of
# machine.installer-rehearse, the rehearsals of the Lenovo's install sitting
# on a disk of their own.
#
# Usage: rehearse_fixture.sh DISK SECTORS GROWSET MKEXFATFIXTURE MKMBR MKGPT
#            ETUDE DIR
#
# DISK is the image the stick installs and SECTORS the target disk's length.
# Run from the root of the tree, it writes into DIR:
#
#   head.vol      the first 128 MiB of the image's root, its third entry, which
#                 growset reads for the grow's ranges
#   params        `root_start N` and `disk_sectors N`, in sectors
#   ranges        the save set, `START COUNT` a line in sectors: the image's
#                 extent, the grow's ranges beyond it, and the disk's end from
#                 the eight-sector boundary at or below the GUID table's backup
#   base.qcow2    the target disk, a qcow2 image whose only data is the first
#                 sector of each range saying `range START COUNT`, so that a
#                 save that read another offset or a restore that put zeros
#                 back shows in the digests
#   save.img      the save disk: an MBR and an exFAT volume from sector 2048
#                 with room for every range, whose one file is the ranges,
#                 written by `ETUDE prepare digest`, which keeps its digest in
#                 the tree's digests file, so that the preparation, which
#                 records it, does not read its gigabytes back to hash them
#
# The target is a qcow2 image, since etude's store reads and hashes every
# byte of an artifact, holes included, and the Lenovo's disk is 250 GB. It is
# written last, under a temporary name, since the rule that runs this script
# makes it and takes it as the sign that the rest is there.
set -eu
disk=$1
sectors=$2
growset=$3
mkexfat=$4
mkmbr=$5
mkgpt=$6
etude=$7
dir=$8
mkdir -p "$dir"
rm -f "$dir/base.qcow2" "$dir/base.qcow2.tmp" "$dir/base.raw" "$dir/base.raw.tmp" "$dir/save.mbr"

# The root's first sector: the third entry of the image's GUID table, whose
# extent `mkgpt --list` prints in bytes.
root_start=$("$mkgpt" --list "$disk" | awk '$1 == 3 { printf "%d\n", $3 / 512; exit }')
[ -n "$root_start" ] && [ "$root_start" -gt 0 ]
image_sectors=$(($(wc -c < "$disk") / 512))
dd if="$disk" of="$dir/head.vol" bs=512 skip="$root_start" count=$((128 * 2048)) 2>/dev/null
printf 'root_start %s\ndisk_sectors %s\n' "$root_start" "$sectors" > "$dir/params"

# The root grows to the sector before the GUID table's backup, 33 sectors
# short of the disk's end. A range inside the image's extent is saved with
# it, and the overlay's map reports writes in clusters of eight sectors, so
# the disk's end is saved from the boundary at or below the backup.
"$growset" --lba "$root_start" "$dir/head.vol" $((sectors - 33 - root_start)) > "$dir/save.set"
backup_from=$(( (sectors - 33) / 8 * 8 ))
{
    echo "0 $image_sectors"
    awk -v n="$image_sectors" -v b="$backup_from" '$1 == "range" && $2 + $3 > n {
        s = $2 < n ? n : $2; e = $2 + $3 < b ? $2 + $3 : b; if (e > s) print s, e - s }' "$dir/save.set"
    echo "$backup_from $((sectors - backup_from))"
} > "$dir/ranges"
rm -f "$dir/save.set"

# The save disk, exFAT and not FAT32, since this kernel writes FAT32 a block
# at a time. mkmbr writes only the table of the first sector, so it writes
# it on an empty disk of the save disk's length, save.mbr, whose first
# mebibyte then goes before the volume as save.img is written.
save_sectors=$(( (image_sectors / 2048 + 2048 + 32) * 2048 ))
"$mkexfat" --make "$dir/save.vol" --sectors "$save_sectors" --offset 2048 \
    --add "ranges=$dir/ranges" > /dev/null
save_bytes=$((1048576 + $(wc -c < "$dir/save.vol")))
dd if=/dev/zero of="$dir/save.mbr" bs=512 seek=$((save_bytes / 512 - 1)) count=1 2>/dev/null
"$mkmbr" --lba "$dir/save.mbr" "2048:$save_sectors:07" > /dev/null
{ dd if="$dir/save.mbr" bs=1048576 count=1 2>/dev/null; cat "$dir/save.vol"; } |
    "$etude" prepare digest "$dir/save.img" > /dev/null
[ "$(wc -c < "$dir/save.img")" -eq "$save_bytes" ]
rm -f "$dir/save.vol" "$dir/save.mbr"

dd if=/dev/zero of="$dir/base.raw.tmp" bs=512 seek=$((sectors - 1)) count=1 2>/dev/null
while read -r s c; do
    printf 'range %s %s\n' "$s" "$c" |
        dd of="$dir/base.raw.tmp" bs=512 seek="$s" count=1 conv=notrunc 2>/dev/null
done < "$dir/ranges"
qemu-img convert -f raw -O qcow2 -o cluster_size=4096 "$dir/base.raw.tmp" "$dir/base.qcow2.tmp"
rm -f "$dir/base.raw.tmp"
mv "$dir/base.qcow2.tmp" "$dir/base.qcow2"
