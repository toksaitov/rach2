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
# The volume includes the input files and FREE-MIB for metadata and free space,
# rounded up to a mebibyte. The MBR gets three entries: the ISO's file
# system from sector 1, of type 0x17, the volume, of type 0x07, and the ISO's
# EFI boot image, of type 0xEF, which UEFI firmware boots a USB device from
# and which xorriso gave the ISO's own table as its second entry. The boot
# code around the table is GRUB's, as the ISO wrote it.
#
# The volume also carries GRUB's EFI image, `bootx64.efi`, with its digest in
# `bootx64.efi.sha256`, beside the root image: `rach2-install --update` takes
# it from beside the image it is given to convert an MBR disk to boot by UEFI
# as well.
#
# The finished image is judged whole by stickcheck with the kernel's own
# partition rules, and its volume is walked by mkexfatfixture --check.
#
# An EXTRA file, if one is given, is placed in the volume beside the image: the
# root file system an update writes, which `rach2-install --update` takes.
#
# FREE-MIB is four by default; recovery backups need a larger value.
# Usage: mkinstallstick.sh ISO DISK OUT MKEXFATFIXTURE MKMBR STICKCHECK
#                         [EXTRA [FREE-MIB]]
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ISO=$1; DISK=$2; OUT=$3; MKFIX=$4; MKMBR=$5; CHECK=$6; EXTRA=${7:-}
FREE_MIB=${8:-4}

if [ -z "$CHECK" ]; then
    echo "usage: mkinstallstick.sh ISO DISK OUT MKEXFATFIXTURE MKMBR STICKCHECK" >&2
    exit 2
fi

MIB_SECTORS=2048
case "$FREE_MIB" in
    ''|*[!0-9]*|0*) echo 'mkinstallstick: invalid free-space size' >&2; exit 2 ;;
esac
if [ "${#FREE_MIB}" -gt 4 ] || [ "$FREE_MIB" -lt 4 ] || [ "$FREE_MIB" -gt 8192 ]; then
    echo 'mkinstallstick: free space must be 4 through 8192 MiB' >&2
    exit 2
fi
SLACK_SECTORS=$((FREE_MIB * MIB_SECTORS))

name=$(basename "$DISK")
iso_bytes=$(wc -c < "$ISO" | tr -d ' ')
iso_sectors=$(( (iso_bytes + 511) / 512 ))
# The EFI boot image, the second entry of the ISO's own table, in sectors.
efi_start=$(od -An -tu4 -j $((446 + 16 + 8)) -N 4 "$ISO" | tr -d ' ')
efi_sectors=$(od -An -tu4 -j $((446 + 16 + 12)) -N 4 "$ISO" | tr -d ' ')
if [ "$(od -An -tx1 -j $((446 + 16 + 4)) -N 1 "$ISO" | tr -d ' ')" != ef ] ||
    [ "$efi_start" -le 1 ] || [ $((efi_start + efi_sectors)) -gt "$iso_sectors" ]; then
    echo "mkinstallstick: $ISO has no EFI boot image as its second partition" >&2
    exit 1
fi
disk_bytes=$(wc -c < "$DISK" | tr -d ' ')
disk_sectors=$(( (disk_bytes + 511) / 512 ))
extra_sectors=0
if [ -n "$EXTRA" ]; then
    extra_sectors=$(( ( $(wc -c < "$EXTRA" | tr -d ' ') + 511) / 512 ))
fi
# The volume starts at the first 1 MiB boundary at or after the ISO's end.
start=$(( ((iso_sectors + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS ))
# GRUB's EFI image and its digest, beside the root image.
efi_sectors_on_volume=$(( ( $(wc -c < "$REPO/vendor/grub/bootx64.efi" | tr -d ' ') + 511) / 512 + 1 ))
volume=$(( disk_sectors + extra_sectors + efi_sectors_on_volume + SLACK_SECTORS ))
volume=$(( ((volume + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS ))

# A failure at any step leaves neither the half-built image nor its parts.
# Each temporary carries this run's process number, so two builds of one stick
# cannot take each other's file away and leave make to delete the stick both
# of them needed.
NEW="$OUT.$$.new"
VOL="$OUT.$$.vol"
MAN="$OUT.$$.manifest"
EFI="$OUT.$$.bootx64.efi"
EFISUM="$OUT.$$.bootx64.efi.sha256"
trap 'rm -f "$NEW" "$VOL" "$MAN" "$EFI" "$EFISUM"' EXIT
rm -f "$NEW"

# The SHA-256 of FILE, named WHAT in a diagnostic, with the hash's own status
# checked: `shasum | cut` under `set -e` reports the status of `cut`, so a
# hash that was never taken would be written as an empty field and the stick
# would carry a digest nothing can match.
sha256_of() { # sha256_of FILE WHAT
    sum_line=$(shasum -a 256 "$1") || {
        echo "mkinstallstick: $2's hash could not be taken" >&2
        return 1
    }
    sum=${sum_line%% *}
    case "$sum" in
        "" | *[!0-9a-f]*)
            echo "mkinstallstick: $2's hash is '$sum', not hexadecimal digits" >&2
            return 1
            ;;
    esac
    [ "${#sum}" = 64 ] || {
        echo "mkinstallstick: $2's hash is $sum, not 64 hexadecimal digits" >&2
        return 1
    }
    printf '%s\n' "$sum"
}

# The pinned EFI image, copied and the copy verified, as every consumer of a
# pinned GRUB image does.
cp "$REPO/vendor/grub/bootx64.efi" "$EFI"
sh "$REPO/vendor/grub/verify_grub.sh" "bootx64.efi=$EFI"
efi_sum=$(sha256_of "$EFI" bootx64.efi) || exit 1
echo "$efi_sum  bootx64.efi" > "$EFISUM"

# The manifest the installer reads: the image's name, its length in bytes and
# its SHA-256, one field a line so a shell reads it with `read`.
sum=$(sha256_of "$DISK" "the image") || exit 1
{
    echo "image $name"
    echo "bytes $disk_bytes"
    echo "sha256 $sum"
} > "$MAN"

if [ -n "$EXTRA" ]; then
    "$MKFIX" --make "$VOL" --sectors "$volume" --offset "$start" \
        --add "$name=$DISK" --add "rach2-install.manifest=$MAN" \
        --add "$(basename "$EXTRA")=$EXTRA" \
        --add "bootx64.efi=$EFI" --add "bootx64.efi.sha256=$EFISUM" > /dev/null
else
    "$MKFIX" --make "$VOL" --sectors "$volume" --offset "$start" \
        --add "$name=$DISK" --add "rach2-install.manifest=$MAN" \
        --add "bootx64.efi=$EFI" --add "bootx64.efi.sha256=$EFISUM" > /dev/null
fi

cp "$ISO" "$NEW"
# Pad to the volume's start, then place the volume.
dd if=/dev/zero of="$NEW" bs=512 seek="$iso_sectors" count=$((start - iso_sectors)) \
    conv=notrunc 2>/dev/null
dd if="$VOL" of="$NEW" bs=1048576 seek=$((start / MIB_SECTORS)) conv=notrunc 2>/dev/null
rm -f "$VOL" "$MAN" "$EFI" "$EFISUM"
# The three entries. mkmbr writes the table bytes and the signature only.
"$MKMBR" --lba "$NEW" "1:$((efi_start - 1)):17" "$start:$volume:07" \
    "$efi_start:$efi_sectors:ef" > /dev/null

"$CHECK" "$NEW" "$start" "$volume"
"$MKFIX" --check "$NEW" "$start" > /dev/null
mv "$NEW" "$OUT"
trap - EXIT
echo "mkinstallstick: wrote $OUT: recovery at sector 1, $volume sectors of exFAT at $start,"\
     "holding $name of $disk_bytes bytes"
