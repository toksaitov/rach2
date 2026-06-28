#!/usr/bin/env sh

#
# mkdisk_grub.sh: build a BIOS-bootable disk image.
#
# The disk has two MBR partitions: partition 1 is a small FAT boot file system
# holding the kernel and GRUB, partition 2 is a KRFFS image. GRUB's boot.img
# goes in the MBR and core.img in the post-MBR gap (partition 1 starts at
# 1 MiB), so a real machine, or QEMU with no -kernel, boots
# BIOS -> MBR -> GRUB -> the Multiboot kernel, which then mounts KRFFS from
# partition 2 (rach2.root=disk).
#
# The disk and GRUB are assembled in a throwaway Docker container (mtools and
# grub-pc-bin), so no host GRUB toolchain is needed. The KRFFS partition image
# is built first on the host with mkimage.sh. Build the kernel and the
# capstones first (make full).
#
# Usage: ./mkdisk_grub.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

KERNEL="$REPO/build/rach2/kernel"
KRFFS_BIN="$REPO/build/images/grub-krffs.bin"
OUT="$REPO/build/images/os-real-grub.disk"

GAP_MB=1
BOOT_MB=16
KRFFS_MB=47
# One spare MiB so the last partition ends before the device edge (parted
# rejects a partition that ends exactly at the device size).
DISK_MB=$((GAP_MB + BOOT_MB + KRFFS_MB + 1))

if [ ! -f "$KERNEL" ]; then
    echo "mkdisk_grub: kernel not built ($KERNEL); run 'make' first" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "mkdisk_grub: docker required to assemble the disk (make doctor)" >&2
    exit 1
fi

# The KRFFS partition image, sized to partition 2, built with the host tools.
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/os-real.manifest" \
    "$KRFFS_BIN" "$KRFFS_MB"

# The GRUB config lives on the host so the container does not have to escape it.
# Build into a .new sibling and rename only on success, so a failed run never
# leaves a half-written disk that passes a later file-existence check. The trap
# clears both temporaries on any exit.
GRUB_CFG="$REPO/build/images/grub-disk.cfg"
trap 'rm -f "$GRUB_CFG" "$OUT.new"' EXIT
cat > "$GRUB_CFG" <<'CFG'
set timeout=3
set default=0

menuentry "Rach2 (disk)" {
    multiboot /boot/kernel rach2.root=disk
    boot
}

menuentry "Rach2 (disk) - rescue (kernel debug shell)" {
    multiboot /boot/kernel rach2.root=disk rach2.rescue
    boot
}
CFG

# Assemble with mtools (FAT) and a hand embed of GRUB's boot code, never a loop
# mount: loop devices destabilize Docker Desktop. The FAT boot partition is
# formatted in a standalone file and written into the disk; a self-contained
# core.img (its modules built in) goes in the post-MBR gap, so GRUB needs
# nothing else on the partition but the kernel and grub.cfg.
#
# grub-bios-setup cannot run here (it fails to canonicalize the disk file's host
# device on Docker's overlay filesystem), so grub_embed.py writes boot.img and
# core.img and patches their pointers directly, which is all grub-bios-setup
# would have done for a contiguous core.img in the gap.
docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" \
    ubuntu:24.04 sh -e -c "
    apt-get update -qq && \
    apt-get install -y -qq grub-pc-bin grub-common parted mtools python3 \
        >/dev/null 2>&1
    export MTOOLS_SKIP_CHECK=1
    export PATH=\"\$PATH:/usr/sbin:/sbin\"
    dd if=/dev/zero of='$OUT.new' bs=1M count=$DISK_MB status=none
    parted -s '$OUT.new' mklabel msdos
    parted -s '$OUT.new' mkpart primary fat32 ${GAP_MB}MiB \
        $((GAP_MB + BOOT_MB))MiB
    parted -s '$OUT.new' mkpart primary $((GAP_MB + BOOT_MB))MiB \
        $((GAP_MB + BOOT_MB + KRFFS_MB))MiB
    parted -s '$OUT.new' set 1 boot on

    dd if=/dev/zero of=/tmp/p1.img bs=1M count=$BOOT_MB status=none
    mformat -i /tmp/p1.img -F ::
    mmd -i /tmp/p1.img ::/boot
    mmd -i /tmp/p1.img ::/boot/grub
    mcopy -i /tmp/p1.img '$KERNEL' ::/boot/kernel
    mcopy -i /tmp/p1.img '$GRUB_CFG' ::/boot/grub/grub.cfg
    dd if=/tmp/p1.img of='$OUT.new' bs=1M seek=$GAP_MB conv=notrunc status=none

    grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' -o /tmp/core.img \
        biosdisk part_msdos fat normal multiboot configfile
    python3 '$REPO/tools/grub_embed.py' '$OUT.new' \
        /usr/lib/grub/i386-pc/boot.img /tmp/core.img

    dd if='$KRFFS_BIN' of='$OUT.new' bs=1M seek=$((GAP_MB + BOOT_MB)) \
        conv=notrunc status=none
    mv '$OUT.new' '$OUT'
"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
