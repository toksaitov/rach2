#!/bin/sh
#
# mkdebian.sh: make the KVM suite's Debian guest, Debian's kernel and an
# initramfs of Debian's static BusyBox whose /init prints its token and
# powers off.
#
# Usage: mkdebian.sh OUTDIR
#
# Writes OUTDIR/vmlinuz, the kernel of debian.txt's kernel package, and
# OUTDIR/initrd, an uncompressed newc archive of /bin/busybox, from
# debian.txt's BusyBox package, and debian-init as /init. MKCPIO names the
# archive writer, tests/system/wifi/mkcpio.c built. Each package is fetched
# once into the clone's build/cache/gnu and checked against its digest on
# every build. The outputs are written beside their names and renamed into
# place, so a failed build leaves the last good guest.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../../.." && pwd)"
GNU="$REPO/vendor/gnu"
MKCPIO="${MKCPIO:-$REPO/build/tools/wifi/mkcpio}"
. "$GNU/debian/fetch_asset.sh"

if [ $# -ne 1 ]; then
    echo "usage: mkdebian.sh OUTDIR" >&2
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
work="$(mktemp -d "$REPO/build/tmp/kvm-debian.XXXXXX")"

# member DEB PATH DIR: extract PATH of the data of package DEB into DIR.
member()
{
    mkdir -p "$3"
    if ! ar p "$1" data.tar.xz | tar -xJf - -C "$3" "$2"; then
        echo "mkdebian: ${1##*/}: no $2" >&2
        return 1
    fi
}

kernel=$(asset debian-guest-kernel "$DIR/debian.txt")
busybox=$(asset debian-guest-busybox "$DIR/debian.txt")
release=${kernel##*/linux-image-}
release=${release%%-unsigned_*}
member "$kernel" "./boot/vmlinuz-$release" "$work/kernel"
member "$busybox" ./usr/bin/busybox "$work/busybox"

printf 'd\t755\tbin\nd\t755\tproc\nd\t755\tsys\nd\t755\tdev\n' > "$work/list"
printf 'f\t755\tbin/busybox\t%s\n' "$work/busybox/usr/bin/busybox" >> "$work/list"
printf 'f\t755\tinit\t%s\n' "$DIR/debian-init" >> "$work/list"
"$MKCPIO" "$OUT/initrd.tmp" "$work/list"
cp "$work/kernel/boot/vmlinuz-$release" "$OUT/vmlinuz.tmp"
mv "$OUT/vmlinuz.tmp" "$OUT/vmlinuz"
mv "$OUT/initrd.tmp" "$OUT/initrd"
echo "mkdebian: result: Debian $release with BusyBox in $OUT"
