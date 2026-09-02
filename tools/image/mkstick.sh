#!/usr/bin/env sh
#
# mkstick.sh: the stick image, a bootable hybrid ISO followed by an exFAT
# volume the machine it boots can mount as its own second partition.
#
# The image is the metal hybrid ISO as mkiso.sh made it, kept whole with its
# boot code, padded to the next 1 MiB, and followed by a 16 MiB exFAT volume
# built by mkexfatfixture with its PartitionOffset equal to the sector it is
# placed at. The MBR gets two entries in the 64 table bytes, and the boot
# code around them is left as GRUB wrote it: the first covers the ISO from
# sector 1, of type 0x17, which is the type isohybrid writes for an ISO9660
# area, and the second is the volume, of type 0x07. The ISO's hybrid MBR
# has no entries of its own, which phase 1 of milestone 40 read and recorded,
# so nothing is kept from it but the code. The image is MBR-only: a GUID
# table written to a larger stick leaves its backup header short of the
# last sector.
#
# The finished image is judged whole by stickcheck, with the kernel's own
# partition rules, and its volume is walked by mkexfatfixture --check.
#
# Usage: mkstick.sh ISO OUT MKEXFATFIXTURE MKMBR STICKCHECK
set -e
ISO=$1; OUT=$2; MKFIX=$3; MKMBR=$4; CHECK=$5
VOLUME_SECTORS=32768
MIB_SECTORS=2048

iso_bytes=$(wc -c < "$ISO" | tr -d ' ')
iso_sectors=$(( (iso_bytes + 511) / 512 ))
# The volume starts at the first 1 MiB boundary at or after the ISO's end.
start=$(( ((iso_sectors + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS ))

# A failure at any step leaves neither the half-built image nor the volume.
trap 'rm -f "$OUT.new" "$OUT.vol"' EXIT
rm -f "$OUT.new"
cp "$ISO" "$OUT.new"
# Pad to the volume's start, then place the volume.
dd if=/dev/zero of="$OUT.new" bs=512 seek="$iso_sectors" count=$((start - iso_sectors)) conv=notrunc 2>/dev/null
"$MKFIX" "$OUT.vol" "--offset=$start" > /dev/null
dd if="$OUT.vol" of="$OUT.new" bs=512 seek="$start" conv=notrunc 2>/dev/null
rm -f "$OUT.vol"
# The two entries. mkmbr writes the table bytes and the signature only.
"$MKMBR" --lba "$OUT.new" "1:$((iso_sectors - 1)):17" "$start:$VOLUME_SECTORS:07" > /dev/null

"$CHECK" "$OUT.new" "$start" "$VOLUME_SECTORS"
"$MKFIX" --check "$OUT.new" "$start" > /dev/null
mv "$OUT.new" "$OUT"
trap - EXIT
echo "mkstick: $OUT: iso $iso_sectors sectors, volume at $start for $VOLUME_SECTORS"
