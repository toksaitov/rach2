#!/usr/bin/env sh

#
# mkiso.sh: build a bootable ISO for one RAM-disk image, for real hardware.
#
# Stages the kernel and the image as GRUB multiboot modules and assembles
# build/images/<image>.iso with xorriso and the pinned GRUB El Torito and
# hybrid images under vendor/grub/, so no GRUB toolchain and no Docker is
# needed. The hybrid MBR keeps the ISO bootable when written to a USB
# device. Each image gets its own ISO and its own staging dir, so
# `make isos` can build all three in parallel without a race.
#
# Usage: ./mkiso.sh <test|os-simple|os-real|rach2-metal>
#
# Requires xorriso. Build the kernel and the image first (make).
#

set -e

IMAGE="$1"
# Allowlist the image name before it reaches the destructive rm -rf below.
case "$IMAGE" in
    test|os-simple|os-real|rach2-metal|rach2-recovery) ;;
    *)
        echo "usage: mkiso.sh <test|os-simple|os-real|rach2-metal|rach2-recovery>" >&2
        exit 2
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

# The metal profile has the kernel that cannot write a disk, and no other
# profile does. This is the stick that goes into a laptop whose SSD has
# somebody's Windows, so the artifact is checked here, not assumed:
# staging the ordinary kernel under the metal name would produce exactly the
# image this milestone promises not to make.
if [ "$IMAGE" = rach2-metal ]; then
    KERNEL="$REPO/build/rach2-metal/kernel"
    POLICY=read-only
elif [ "$IMAGE" = rach2-recovery ]; then
    # The recovery kernel and its RAM image on a stick: a machine boots it
    # without its disk and reaches reflash. The disk policy is read-write,
    # since writing the disk is what recovery is for.
    KERNEL="$REPO/build/rach2-recovery/kernel"
    POLICY=read-write
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
if ! sh "$REPO/tools/inspect/check_diskpolicy.sh" "$KERNEL" "$POLICY"; then
    echo "mkiso: refusing to build $IMAGE.iso around a kernel with the wrong disk policy" >&2
    exit 1
fi
if [ ! -f "$KRFFS" ]; then
    echo "mkiso: image not built ($KRFFS); run 'make $IMAGE-image' first" >&2
    exit 1
fi

rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub"
trap 'rm -rf "$ISODIR" "$ISODIR.hybrid.img" "$ISODIR.log" "$ISO_OUT.new"' EXIT
# cat (not cp) so the modules are dense: some GRUB builds crash on sparse files.
cat "$KERNEL" > "$ISODIR/boot/kernel"
cat "$KRFFS" > "$ISODIR/boot/$IMAGE.krffs"

# The metal profile's menu. It is longer than the others' because this is the
# stick that goes into a laptop with no serial port, where every question has
# to be answerable by choosing a menu entry, not by typing a command
# line at a machine you cannot read.
#
# The default entry's address is baked in, not typed, because a machine
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
    METAL_ENTRIES="menuentry \"Start Rach2, and probe the disk controller\" {
    multiboot /boot/kernel $METAL_NET rach2.disk=ahci rach2.diskdiag
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Start Rach2, and hold the diagnostics on screen\" {
    multiboot /boot/kernel $METAL_NET rach2.diag
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Start Rach2, and ask the network for an address\" {
    multiboot /boot/kernel rach2.ip=none rach2.netconsole
    module /boot/rach2-metal.krffs
    boot
}

menuentry \"Start Rach2 with no network\" {
    multiboot /boot/kernel
    module /boot/rach2-metal.krffs
    boot
}
"
    METAL_DEFAULT_ARGS="$METAL_NET"
elif [ "$IMAGE" = rach2-recovery ]; then
    # The stick takes an address from DHCP and answers SSH on it, which is
    # how reflash is reached from a host beside the machine; the disk it
    # repairs is behind an AHCI controller on the machine this is for. The
    # default entry repairs nothing: `rach2.fsck`, which makes init run
    # `e2fsck -p` on every ext2 partition first, is the second entry's.
    METAL_ENTRIES="menuentry \"Install or repair, and check the disks first\" {
    multiboot /boot/kernel rach2.ip=none rach2.disk=ahci rach2.fsck
    module /boot/$IMAGE.krffs
    boot
}
"
    METAL_DEFAULT_ARGS='rach2.ip=none rach2.disk=ahci'
else
    METAL_ENTRIES=''
    METAL_DEFAULT_ARGS=''
fi

# The first entry is what a person is here to do, said in those words: the
# recovery stick is how a machine is installed and repaired, and the others
# are that system started for their own reasons.
if [ "$IMAGE" = rach2-recovery ]; then
    FIRST_TITLE="Install Rach2, or repair a computer"
else
    FIRST_TITLE="Start Rach2 ($IMAGE)"
fi

cat > "$ISODIR/boot/grub/grub.cfg" <<CFG
set timeout=3
set default=0

menuentry "$FIRST_TITLE" {
    multiboot /boot/kernel $METAL_DEFAULT_ARGS
    module /boot/$IMAGE.krffs
    boot
}

$METAL_ENTRIES
menuentry "Start the kernel debug shell" {
    multiboot /boot/kernel rach2.rescue
    module /boot/$IMAGE.krffs
    boot
}
CFG

command -v xorriso >/dev/null 2>&1 || {
    echo "mkiso: xorriso is required (brew install xorriso, or apt install xorriso)" >&2
    exit 1
}

# The pinned El Torito and hybrid images: copied into this build's private
# staging first, then the copies verified, so the bytes checked are the
# bytes consumed and a regeneration racing this build cannot slip a file
# between the check and the read.
cp "$REPO/vendor/grub/eltorito.img" "$ISODIR/boot/grub/eltorito.img"
cp "$REPO/vendor/grub/boot-hybrid.img" "$ISODIR.hybrid.img"
sh "$REPO/vendor/grub/verify_grub.sh" \
    "eltorito.img=$ISODIR/boot/grub/eltorito.img" \
    "boot-hybrid.img=$ISODIR.hybrid.img"

# xorriso assembles what grub-mkrescue assembled in the retired container:
# an ISO9660 tree with the El Torito boot image, its boot-info table patched
# in, and GRUB's hybrid MBR so the same ISO boots from a USB device. The
# output goes to a .new sibling renamed only on success, and the exit status
# is xorriso's own, not a log filter's, so a failure can neither pass nor
# leave a half-written ISO where the old one was.
if xorriso -as mkisofs -graft-points -R -J \
        -b boot/grub/eltorito.img -no-emul-boot -boot-load-size 4 \
        -boot-info-table --grub2-boot-info \
        --grub2-mbr "$ISODIR.hybrid.img" \
        -o "$ISO_OUT.new" "$ISODIR" > "$ISODIR.log" 2>&1; then
    grep -v '^xorriso : UPDATE' "$ISODIR.log" >&2 || true
else
    cat "$ISODIR.log" >&2
    echo "mkiso: the ISO build failed (see the diagnostics above)" >&2
    exit 1
fi
mv "$ISO_OUT.new" "$ISO_OUT"

echo "mkiso: wrote $ISO_OUT"
echo "mkiso: boot it with rach2/run_from_iso.sh $IMAGE, or burn it to a USB stick"
