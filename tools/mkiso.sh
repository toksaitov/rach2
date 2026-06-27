#!/usr/bin/env sh

#
# mkiso.sh: build a bootable ISO for one RAM-disk image, for real hardware.
#
# Stages the kernel and the image as GRUB multiboot modules and runs
# grub-mkrescue (in a throwaway Docker container, so no host GRUB toolchain is
# needed) to produce build/images/<image>.iso. Each image gets its own ISO and its
# own staging dir, so `make isos` can build all three in parallel without a race.
#
# Usage: ./mkiso.sh <test|os-simple|os-real>
#
# Requires Docker (the only step that does). Build the kernel and the image first
# (make / make <image>-image).
#

set -e

IMAGE="$1"
if [ -z "$IMAGE" ]; then
    echo "usage: mkiso.sh <test|os-simple|os-real>" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL="$REPO/build/rach2/kernel"
KRFFS="$REPO/build/images/$IMAGE.krffs"
ISO_OUT="$REPO/build/images/$IMAGE.iso"
ISODIR="$REPO/build/iso/$IMAGE"

if [ ! -f "$KERNEL" ]; then
    echo "mkiso: kernel not built ($KERNEL); run 'make' first" >&2
    exit 1
fi
if [ ! -f "$KRFFS" ]; then
    echo "mkiso: image not built ($KRFFS); run 'make $IMAGE-image' first" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "mkiso: docker is required for grub-mkrescue; install it (make doctor)" >&2
    exit 1
fi

rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub"
# cat (not cp) so the modules are dense: some GRUB builds crash on sparse files.
cat "$KERNEL" > "$ISODIR/boot/kernel"
cat "$KRFFS" > "$ISODIR/boot/$IMAGE.krffs"

cat > "$ISODIR/boot/grub/grub.cfg" <<CFG
set timeout=3
set default=0

menuentry "Rach2 ($IMAGE)" {
    multiboot /boot/kernel
    module /boot/$IMAGE.krffs
    boot
}
CFG

# Mount the repo at its own absolute path so the absolute build/ paths resolve in
# the container; grub-mkrescue gets absolute, quoted paths.
docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" ubuntu:24.04 sh -c \
    "apt-get update -qq && \
     apt-get install -y -qq grub-pc-bin grub-common xorriso mtools >/dev/null 2>&1 && \
     grub-mkrescue -o '$ISO_OUT' '$ISODIR' 2>/dev/null"

rm -rf "$ISODIR"
echo "mkiso: wrote $ISO_OUT"
echo "mkiso: boot it with rach2/run_from_iso.sh $IMAGE, or burn it to a USB stick"
