#!/usr/bin/env sh

#
# mkiso.sh: build a bootable ISO for one RAM-disk image, for real hardware.
#
# Stages the kernel and the image as GRUB multiboot modules and runs
# grub-mkrescue (in a throwaway Docker container, so no host GRUB toolchain is
# needed) to produce build/images/<image>.iso. Each image gets its own ISO and its
# own staging dir, so `make isos` can build all three in parallel without a race.
#
# Usage: ./mkiso.sh <test|os-simple|os-real|rach2-metal>
#
# Requires Docker (the only step that does). Build the kernel and the image first
# (make / make <image>-image).
#

set -e

IMAGE="$1"
# Allowlist the image name before it reaches the destructive rm -rf below.
case "$IMAGE" in
    test|os-simple|os-real|rach2-metal) ;;
    *)
        echo "usage: mkiso.sh <test|os-simple|os-real|rach2-metal>" >&2
        exit 2
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
# The metal profile carries the kernel that cannot write a disk, and no other
# profile does. This is the stick that goes into a laptop whose SSD holds
# somebody's Windows, so the artifact is checked here rather than assumed:
# staging the ordinary kernel under the metal name would produce exactly the
# image this milestone promises not to make.
if [ "$IMAGE" = rach2-metal ]; then
    KERNEL="$REPO/build/rach2-metal/kernel"
    POLICY=read-only
else
    KERNEL="$REPO/build/rach2/kernel"
    POLICY=read-write
fi
KRFFS="$REPO/build/images/$IMAGE.krffs"
ISO_OUT="$REPO/build/images/$IMAGE.iso"
ISODIR="$REPO/build/iso/$IMAGE"

if [ ! -f "$KERNEL" ]; then
    if [ "$IMAGE" = rach2-metal ]; then
        echo "mkiso: metal kernel not built ($KERNEL); run 'make rach2-metal' first" >&2
    else
        echo "mkiso: kernel not built ($KERNEL); run 'make' first" >&2
    fi
    exit 1
fi
if ! python3 "$REPO/tools/check_diskpolicy.py" "$KERNEL" "$POLICY"; then
    echo "mkiso: refusing to build $IMAGE.iso around a kernel with the wrong disk policy" >&2
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

# The metal profile gets a third entry: a machine with no serial port answers
# only on its screen, so the numbers a first bring-up needs (the module layout
# the bootloader chose, what memory survived, which Ethernet silicon is in the
# slot) are one menu selection away rather than a rebuild away.
if [ "$IMAGE" = rach2-metal ]; then
    DIAG_ENTRY='menuentry "Rach2 (metal) - diagnostics" {
    multiboot /boot/kernel rach2.diag
    module /boot/rach2-metal.krffs
    boot
}
'
else
    DIAG_ENTRY=''
fi

cat > "$ISODIR/boot/grub/grub.cfg" <<CFG
set timeout=3
set default=0

menuentry "Rach2 ($IMAGE)" {
    multiboot /boot/kernel
    module /boot/$IMAGE.krffs
    boot
}

$DIAG_ENTRY
menuentry "Rach2 ($IMAGE) - rescue (kernel debug shell)" {
    multiboot /boot/kernel rach2.rescue
    module /boot/$IMAGE.krffs
    boot
}
CFG

# Mount the repo at its own absolute path so the absolute build/ paths resolve in
# the container; grub-mkrescue gets absolute, quoted paths. grub-mkrescue's stderr
# is kept (its diagnostics name a missing module or a broken layout), and a build
# failure is fatal rather than a silent "wrote" line over a missing or stale ISO.
if ! docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" ubuntu:24.04 sh -c \
    "apt-get update -qq && \
     apt-get install -y -qq grub-pc-bin grub-common xorriso mtools >/dev/null 2>&1 && \
     grub-mkrescue -o '$ISO_OUT' '$ISODIR'"; then
    echo "mkiso: the ISO build failed in the container (see the diagnostics above)" >&2
    rm -rf "$ISODIR"
    exit 1
fi

rm -rf "$ISODIR"
echo "mkiso: wrote $ISO_OUT"
echo "mkiso: boot it with rach2/run_from_iso.sh $IMAGE, or burn it to a USB stick"
