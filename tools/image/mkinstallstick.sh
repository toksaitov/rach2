#!/usr/bin/env sh
#
# mkinstallstick.sh: the stick a person installs Rach2 from.
#
# It has the shape of the product stick mkstick.sh writes, with two
# differences: the bootable part is the recovery system rather than the metal
# one, since recovery is what runs the installer, and the exFAT volume after it
# carries a whole disk image, under its own name, and a manifest naming that
# image's name, length and SHA-256. The product stick carries the sealed disk;
# the suites build one carrying a small disk instead, so the rows that install
# do not each write a gibibyte and a half.
#
# The installer reads the manifest, hashes the image on the stick before it
# writes anything, and hashes the disk after it has written it, so a stick
# damaged in either direction is found rather than installed.
#
# The volume is as large as the image plus four megabytes for the file system's
# own structures, rounded up to a mebibyte. The MBR gets two entries: the ISO
# from sector 1, of type 0x17, and the volume, of type 0x07. The boot code
# around the table is GRUB's, as the ISO wrote it.
#
# The finished image is judged whole by stickcheck with the kernel's own
# partition rules, and its volume is walked by mkexfatfixture --check.
#
# An EXTRA file, if one is given, is placed in the volume beside the image: the
# root file system an update writes, which `rach2-install --update` takes.
#
# Usage: mkinstallstick.sh ISO DISK OUT MKEXFATFIXTURE MKMBR STICKCHECK [EXTRA]
set -e
ISO=$1; DISK=$2; OUT=$3; MKFIX=$4; MKMBR=$5; CHECK=$6; EXTRA=${7:-}

if [ -z "$CHECK" ]; then
    echo "usage: mkinstallstick.sh ISO DISK OUT MKEXFATFIXTURE MKMBR STICKCHECK" >&2
    exit 2
fi

MIB_SECTORS=2048
SLACK_SECTORS=8192

name=$(basename "$DISK")
iso_bytes=$(wc -c < "$ISO" | tr -d ' ')
iso_sectors=$(( (iso_bytes + 511) / 512 ))
disk_bytes=$(wc -c < "$DISK" | tr -d ' ')
disk_sectors=$(( (disk_bytes + 511) / 512 ))
extra_sectors=0
if [ -n "$EXTRA" ]; then
    extra_sectors=$(( ( $(wc -c < "$EXTRA" | tr -d ' ') + 511) / 512 ))
fi
# The volume starts at the first 1 MiB boundary at or after the ISO's end.
start=$(( ((iso_sectors + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS ))
volume=$(( disk_sectors + extra_sectors + SLACK_SECTORS ))
volume=$(( ((volume + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS ))

# A failure at any step leaves neither the half-built image nor its parts.
trap 'rm -f "$OUT.new" "$OUT.vol" "$OUT.manifest"' EXIT
rm -f "$OUT.new"

# The manifest the installer reads: the image's name, its length in bytes and
# its SHA-256, one field a line so a shell reads it with `read`.
sum=$(shasum -a 256 "$DISK" | cut -d' ' -f1)
{
    echo "image $name"
    echo "bytes $disk_bytes"
    echo "sha256 $sum"
} > "$OUT.manifest"

if [ -n "$EXTRA" ]; then
    "$MKFIX" --make "$OUT.vol" --sectors "$volume" --offset "$start" \
        --add "$name=$DISK" --add "rach2-install.manifest=$OUT.manifest" \
        --add "$(basename "$EXTRA")=$EXTRA" > /dev/null
else
    "$MKFIX" --make "$OUT.vol" --sectors "$volume" --offset "$start" \
        --add "$name=$DISK" --add "rach2-install.manifest=$OUT.manifest" > /dev/null
fi

cp "$ISO" "$OUT.new"
# Pad to the volume's start, then place the volume.
dd if=/dev/zero of="$OUT.new" bs=512 seek="$iso_sectors" count=$((start - iso_sectors)) \
    conv=notrunc 2>/dev/null
dd if="$OUT.vol" of="$OUT.new" bs=1048576 seek=$((start / MIB_SECTORS)) conv=notrunc 2>/dev/null
rm -f "$OUT.vol" "$OUT.manifest"
# The two entries. mkmbr writes the table bytes and the signature only.
"$MKMBR" --lba "$OUT.new" "1:$((iso_sectors - 1)):17" "$start:$volume:07" > /dev/null

"$CHECK" "$OUT.new" "$start" "$volume"
"$MKFIX" --check "$OUT.new" "$start" > /dev/null
mv "$OUT.new" "$OUT"
trap - EXIT
echo "mkinstallstick: wrote $OUT: recovery at sector 1, $volume sectors of exFAT at $start,"\
     "holding $name of $disk_bytes bytes"
