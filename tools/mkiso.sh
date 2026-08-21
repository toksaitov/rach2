#!/usr/bin/env sh

#
# mkiso.sh: build a bootable ISO for one RAM-disk image, for real hardware.
#
# Stages the kernel and the image as GRUB multiboot modules and runs
# grub-mkrescue (in a throwaway Docker container, so no host GRUB toolchain is
# needed) to produce build/images/<image>.iso. Each image gets its own ISO and
# its own staging dir, so `make isos` can build all three in parallel without a
# race.
#
# Usage: ./mkiso.sh <test|os-simple|os-real|rach2-metal>
#
# Requires Docker (the only step that does). Build the kernel and the image
# first (make / make <image>-image).
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
# The metal profile has the kernel that cannot write a disk, and no other
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

# The metal profile's menu. It is longer than the others' because this is the
# stick that goes into a laptop with no serial port, where every question has
# to be answerable by choosing a menu entry rather than by typing a command
# line at a machine you cannot read.
#
# The default entry's address is baked in rather than typed, because a machine
# whose network is the broken part is one you want on a known address. Typing
# one at a GRUB prompt on a laptop keyboard, correctly, every time, is a tax
# with no teaching value. These defaults are the network the machine actually
# lives on; override them at build time for anywhere else:
#
# RACH2_METAL_IP=10.0.0.50/24 RACH2_METAL_GATEWAY=10.0.0.1 make rach2-metal-iso
#
# The entry after it takes an address from whatever DHCP server the network has,
# which is what a stick carried between two buildings needs: `rach2.ip=none`
# leaves IPv4 unconfigured at boot, and init then runs `ifup -a`, which reads
# `/etc/network/interfaces` and runs the client. It is not the default, because
# the default entry is the one for the network this stick was built for. The
# last entry boots with no network configuration at all.
METAL_IP="${RACH2_METAL_IP:-192.168.0.105/24}"
METAL_GATEWAY="${RACH2_METAL_GATEWAY:-192.168.0.1}"
METAL_NET="rach2.ip=$METAL_IP rach2.gateway=$METAL_GATEWAY rach2.netconsole"

if [ "$IMAGE" = rach2-metal ]; then
    # The disk diagnostic is its own entry and not part of the default, because
    # it probes the SATA controller and resets its link, and that path has less
    # standing on this board than the rest. A boot that stops there should cost
    # a different menu choice, not a reinstall.
    METAL_ENTRIES="menuentry \"Rach2 (metal) - with the disk diagnostic\" {
    multiboot /boot/kernel $METAL_NET rach2.disk=ahci rach2.diskdiag
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Rach2 (metal) - diagnostics, held on screen\" {
    multiboot /boot/kernel $METAL_NET rach2.diag
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Rach2 (metal) - address from DHCP\" {
    multiboot /boot/kernel rach2.ip=none rach2.netconsole
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Rach2 (metal) - no network configuration\" {
    multiboot /boot/kernel
    module /boot/rach2-metal.krffs
    boot
}
"
    METAL_DEFAULT_ARGS="$METAL_NET"
else
    METAL_ENTRIES=''
    METAL_DEFAULT_ARGS=''
fi

cat > "$ISODIR/boot/grub/grub.cfg" <<CFG
set timeout=3
set default=0

menuentry "Rach2 ($IMAGE)" {
    multiboot /boot/kernel $METAL_DEFAULT_ARGS
    module /boot/$IMAGE.krffs
    boot
}

$METAL_ENTRIES
menuentry "Rach2 ($IMAGE) - rescue (kernel debug shell)" {
    multiboot /boot/kernel rach2.rescue
    module /boot/$IMAGE.krffs
    boot
}
CFG

# Mount the repo at its own absolute path so the absolute build/ paths resolve
# in the container; grub-mkrescue gets absolute, quoted paths. grub-mkrescue's
# stderr is kept (its diagnostics name a missing module or a broken layout), and
# a build failure is fatal rather than a silent "wrote" line over a missing or
# stale ISO.
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
