#!/bin/sh
#
# mklinuxhost.sh: make the Linux host of the KVM exit counts, Debian's
# kernel of debian.txt with an initramfs of Debian's static BusyBox, its
# AHCI and KVM modules and linux-host-init as /init, which runs the guest
# boots of exits.sh on the GNU root with Linux's KVM in Rach2's place.
#
# Usage: mklinuxhost.sh OUTDIR
#
# Writes OUTDIR/vmlinuz and OUTDIR/initrd. Boot them under the same outer
# QEMU as Rach2, with the GNU root's disk first and the KVM suite's data
# disk, kvm.ext2, second, both on AHCI, and `console=ttyS0`; the console
# then carries exits.sh's lines for Linux. MKCPIO names the archive writer,
# tests/system/wifi/mkcpio.c built. The packages are fetched once into the
# clone's build/cache/gnu and checked against their digests, as mkdebian.sh
# fetches them.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../../.." && pwd)"
GNU="$REPO/vendor/gnu"
MKCPIO="${MKCPIO:-$REPO/build/tools/wifi/mkcpio}"
. "$GNU/debian/fetch_asset.sh"

# The modules linux-host-init loads, in the order it loads them, each after
# those it depends on.
MODULES="scsi_common scsi_mod libata libahci ahci sd_mod irqbypass kvm kvm-amd"

if [ $# -ne 1 ]; then
    echo "usage: mklinuxhost.sh OUTDIR" >&2
    exit 2
fi
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
case $MKCPIO in
    /*) ;;
    *) MKCPIO="$PWD/$MKCPIO" ;;
esac

work=
trap '[ -z "$work" ] || rm -rf "$work"; rm -f "$OUT/vmlinuz.tmp" "$OUT/initrd.tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp"
work="$(mktemp -d "$REPO/build/tmp/kvm-linuxhost.XXXXXX")"

kernel=$(asset debian-guest-kernel "$DIR/debian.txt")
busybox=$(asset debian-guest-busybox "$DIR/debian.txt")
release=${kernel##*/linux-image-}
release=${release%%-unsigned_*}
mkdir -p "$work/kernel" "$work/busybox" "$work/modules"
ar p "$kernel" data.tar.xz | tar -xJf - -C "$work/kernel" "./boot/vmlinuz-$release" \
    "./usr/lib/modules/$release/kernel"
ar p "$busybox" data.tar.xz | tar -xJf - -C "$work/busybox" ./usr/bin/busybox

{
    printf 'd\t755\tbin\nd\t755\tproc\nd\t755\tsys\nd\t755\tdev\nd\t755\troot\n'
    printf 'd\t755\tgnu\nd\t755\tlib\nd\t755\tlib/modules\n'
    printf 'f\t755\tbin/busybox\t%s\n' "$work/busybox/usr/bin/busybox"
    printf 'f\t755\tinit\t%s\n' "$DIR/linux-host-init"
    for m in $MODULES; do
        f=$(find "$work/kernel/usr/lib/modules/$release/kernel" -name "$m.ko.xz")
        if [ -z "$f" ]; then
            echo "mklinuxhost: no module $m in $release" >&2
            exit 1
        fi
        xz -dc "$f" > "$work/modules/$m.ko"
        printf 'f\t644\tlib/modules/%s.ko\t%s\n' "$m" "$work/modules/$m.ko"
    done
} > "$work/list"
"$MKCPIO" "$OUT/initrd.tmp" "$work/list"
cp "$work/kernel/boot/vmlinuz-$release" "$OUT/vmlinuz.tmp"
mv "$OUT/vmlinuz.tmp" "$OUT/vmlinuz"
mv "$OUT/initrd.tmp" "$OUT/initrd"
echo "mklinuxhost: result: Debian $release with BusyBox and $MODULES in $OUT"
