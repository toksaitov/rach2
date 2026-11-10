#!/bin/sh
# Make the assignment guest from the KVM suite's pinned Debian packages.
# Usage: mkguest.sh OUTDIR
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
GNU=$REPO/vendor/gnu
. "$GNU/debian/fetch_asset.sh"
[ "$#" = 1 ] || exit 2
out=$1
work=
trap '[ -z "$work" ] || rm -rf "$work"; rm -f "$out/initrd.tmp"' EXIT
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
