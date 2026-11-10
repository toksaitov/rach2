#!/bin/sh
# Make the assignment guest from the KVM suite's pinned Debian packages:
# OUTDIR/initrd, and OUTDIR/grub.img, the guest's own disk, a FAT volume
# that UEFI firmware boots from its removable-media path, with Debian's
# GRUB (guest.txt), its menu (grub.cfg), the kernel and the initramfs.
# Usage: mkguest.sh OUTDIR
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
GNU=$REPO/vendor/gnu
. "$GNU/debian/fetch_asset.sh"
[ "$#" = 1 ] || exit 2
out=$1
work=
trap '[ -z "$work" ] || rm -rf "$work"; rm -f "$out/initrd.tmp" "$out/grub.img.tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$REPO/build/tmp"
work=$(mktemp -d "$REPO/build/tmp/assign-guest.XXXXXX")
kernel=$(asset debian-guest-kernel "$REPO/tests/kvm/guests/debian.txt")
busybox=$(asset debian-guest-busybox "$REPO/tests/kvm/guests/debian.txt")
ar p "$kernel" data.tar.xz | tar -xJf - -C "$work"
ar p "$busybox" data.tar.xz | tar -xJf - -C "$work" ./usr/bin/busybox
printf 'd\t755\tbin\nd\t755\tdev\nd\t755\tproc\nd\t755\tsys\nd\t755\tmodules\n' > "$work/list"
printf 'f\t755\tbin/busybox\t%s\nf\t755\tinit\t%s\n' \
    "$work/usr/bin/busybox" "$REPO/tests/assign/guest-init" >> "$work/list"
for name in failover net_failover virtio virtio_ring virtio_pci_modern_dev virtio_pci_legacy_dev virtio_pci virtio_net; do
    module=$(find "$work" -name "$name.ko*" -print)
    if [ -z "$module" ]; then
        if find "$work" -name modules.builtin -exec grep -l "/$name.ko$" {} \; | grep -q .; then
            continue
        fi
        echo "assign-guest: missing module: $name" >&2
        exit 1
    fi
    case $module in
        *.xz) xz -dc "$module" > "$work/$name.ko" ;;
        *.zst) zstd -dc "$module" > "$work/$name.ko" ;;
        *.ko) cp "$module" "$work/$name.ko" ;;
        *) exit 1 ;;
    esac
    printf 'f\t644\tmodules/%s.ko\t%s/%s.ko\n' "$name" "$work" "$name" >> "$work/list"
done
"$REPO/build/tools/wifi/mkcpio" "$out/initrd.tmp" "$work/list"
mv "$out/initrd.tmp" "$out/initrd"

# The disk. Two builds write the same volume: the serial is fixed, and the
# entries' dates come from SOURCE_DATE_EPOCH, which mtools honours.
for t in mformat mmd mcopy; do
    command -v "$t" >/dev/null 2>&1 || { echo "assign-guest: mtools is required" >&2; exit 1; }
done
grub=$(asset debian-guest-grub-efi "$REPO/tests/assign/guest.txt")
ar p "$grub" data.tar.xz | tar -xJf - -C "$work" ./usr/lib/grub/x86_64-efi/monolithic/grubx64.efi
vmlinuz=$(find "$work/boot" -name 'vmlinuz-*' -print)
[ -f "$vmlinuz" ] || { echo "assign-guest: no kernel in its package" >&2; exit 1; }
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1700000000}
export SOURCE_DATE_EPOCH
img=$out/grub.img.tmp
dd if=/dev/zero of="$img" bs=1048576 count=48 2>/dev/null
mformat -i "$img" -N 82470002 -v ASSIGNGUEST ::
mmd -i "$img" ::/EFI ::/EFI/BOOT ::/EFI/debian
mcopy -i "$img" "$work/usr/lib/grub/x86_64-efi/monolithic/grubx64.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$img" "$REPO/tests/assign/grub.cfg" ::/EFI/debian/grub.cfg
mcopy -i "$img" "$vmlinuz" ::/vmlinuz
mcopy -i "$img" "$out/initrd" ::/initrd
mv "$img" "$out/grub.img"
