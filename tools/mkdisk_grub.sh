#!/usr/bin/env sh

#
# mkdisk_grub.sh: build a BIOS-bootable disk image with kernel slots.
#
# The disk has two MBR partitions: partition 1 is a small FAT boot file system
# holding GRUB and two kernels, partition 2 is a KRFFS image. GRUB's boot.img
# goes in the MBR and core.img in the post-MBR gap (partition 1 starts at
# 1 MiB), so a real machine, or QEMU with no -kernel, boots
# BIOS -> MBR -> GRUB -> the Multiboot kernel, which then mounts KRFFS from
# partition 2 (rach2.root=disk).
#
# The boot partition is the install surface (dev-log/23-install/plan.md):
# /boot/kernel is the candidate, the kernel padded with zeros to a fixed
# 2 MiB slot whose data sectors install-kernel rewrites in place on the
# machine itself; /boot/fallback is the same kernel, never written afterward;
# and /boot/grub/grubenv is the 1024-byte state block that selects between
# them, initialized to boot the candidate. The build verifies every staged
# chain and the four files' disjointness before committing the partition,
# because the installer walks the same structures and a broken chain would
# stop every install.
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

# The kernel and output default to the host build's, and both can be named:
# the self-build proof hands the guest-built kernel here and boots the result.
KERNEL="${1:-$REPO/build/rach2/kernel}"
OUT="${2:-$REPO/build/images/os-real-grub.disk}"
KRFFS_BIN="$REPO/build/images/grub-krffs.bin"

# Three knobs for a disk aimed at a particular machine, each defaulting to
# exactly what this script always built so no existing caller changes:
# the manifest staged into the KRFFS partition, that partition's size, and
# extra kernel command-line tokens appended to every menu entry (a real
# machine needs its address and its disk driver named; QEMU's defaults need
# neither).
MANIFEST="${RACH2_DISK_MANIFEST:-$REPO/krffs/manifests/os-real.manifest}"
KRFFS_MB="${RACH2_DISK_KRFFS_MB:-47}"
EXTRA_APPEND="${RACH2_DISK_APPEND:-}"

GAP_MB=1
BOOT_MB=16
# The candidate slot: install-kernel rewrites these bytes in place and never
# touches FAT metadata, so the file's size is fixed at build time.
SLOT_BYTES=2097152
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
sh "$REPO/krffs/mkimage.sh" "$MANIFEST" \
    "$KRFFS_BIN" "$KRFFS_MB"

# The GRUB config lives on the host so the container does not have to escape it.
# Build into a .new sibling and rename only on success, so a failed run never
# leaves a half-written disk that passes a later file-existence check. The trap
# clears both temporaries on any exit.
GRUB_CFG="$REPO/build/images/grub-disk.cfg"
trap 'rm -f "$GRUB_CFG" "$OUT.new"' EXIT

# The install contract (dev-log/23-install/plan.md): /boot/kernel is the
# candidate slot install-kernel rewrites, /boot/fallback the untouched copy,
# and /boot/grub/grubenv the 1024-byte state block both GRUB and the installer
# edit in place. saved_entry and next_entry are compared against the exact
# menu ids and any other value selects the fallback entry, because the block
# has no checksum and a torn write can still load; next_entry is consumed by
# this script (set empty, saved back), the one-boot trial, since this GRUB
# does not consume it built-in. The serial terminal mirrors loader errors
# into the harness's log.
cat > "$GRUB_CFG" <<'CFG'
serial --unit=0 --speed=115200
terminal_output serial console
load_env
set default=fallback
if [ "${saved_entry}" = "candidate" ]; then set default=candidate; fi
if [ "${next_entry}" = "candidate" ]; then
    set default=candidate
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "fallback" ]; then
    set default=fallback
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "debug" ]; then
    set default=debug
    set next_entry=
    save_env next_entry
fi
# The fallback variable takes the entry NUMBER (an id does not resolve), so
# the menu order below is part of this file's contract: candidate 0,
# fallback 1, debug 2.
set fallback=1
set timeout=3

menuentry "Rach2" --id candidate {
    multiboot /boot/kernel rach2.root=disk rach2.slot=candidate
    boot
}

menuentry "Rach2 (fallback kernel)" --id fallback {
    multiboot /boot/fallback rach2.root=disk rach2.slot=fallback
    boot
}

menuentry "Rach2 (kernel debug shell)" --id debug {
    multiboot /boot/fallback rach2.root=disk rach2.slot=fallback rach2.rescue
    boot
}
CFG

# The per-machine tokens ride into every entry the same way, appended after
# the tokens the install contract fixes. A sed over the finished file keeps
# the heredoc above quoted, which keeps GRUB's own ${...} variables out of the
# shell's reach.
if [ -n "$EXTRA_APPEND" ]; then
    sed "s|rach2.root=disk|rach2.root=disk $EXTRA_APPEND|" "$GRUB_CFG" > "$GRUB_CFG.new"
    mv "$GRUB_CFG.new" "$GRUB_CFG"
fi

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
    command -v grub-editenv >/dev/null || {
        echo 'mkdisk_grub: grub-editenv is not in the container' >&2; exit 1; }
    command -v mshowfat >/dev/null || {
        echo 'mkdisk_grub: mshowfat is not in the container' >&2; exit 1; }
    dd if=/dev/zero of='$OUT.new' bs=1M count=$DISK_MB status=none
    parted -s '$OUT.new' mklabel msdos
    parted -s '$OUT.new' mkpart primary fat32 ${GAP_MB}MiB \
        $((GAP_MB + BOOT_MB))MiB
    parted -s '$OUT.new' mkpart primary $((GAP_MB + BOOT_MB))MiB \
        $((GAP_MB + BOOT_MB + KRFFS_MB))MiB
    parted -s '$OUT.new' set 1 boot on

    KBYTES=\$(stat -c %s '$KERNEL')
    if [ \"\$KBYTES\" -gt $SLOT_BYTES ]; then
        echo \"mkdisk_grub: the kernel is \$KBYTES bytes and the slot holds $SLOT_BYTES\" >&2
        exit 1
    fi
    dd if=/dev/zero of=/tmp/candidate.pad bs=$SLOT_BYTES count=1 status=none
    dd if='$KERNEL' of=/tmp/candidate.pad conv=notrunc status=none
    dd if=/dev/zero of=/tmp/p1.img bs=1M count=$BOOT_MB status=none
    mformat -i /tmp/p1.img -F ::
    mmd -i /tmp/p1.img ::/boot
    mmd -i /tmp/p1.img ::/boot/grub
    mcopy -i /tmp/p1.img /tmp/candidate.pad ::/boot/kernel
    mcopy -i /tmp/p1.img '$KERNEL' ::/boot/fallback
    mcopy -i /tmp/p1.img '$GRUB_CFG' ::/boot/grub/grub.cfg
    grub-editenv /tmp/genv create
    grub-editenv /tmp/genv set saved_entry=candidate
    mcopy -i /tmp/p1.img /tmp/genv ::/boot/grub/grubenv
    # Every staged chain, verified before the partition is committed: each of
    # the four files must be whole (its cluster count matching its size) and
    # the four extents disjoint. mshowfat prints cluster ranges; each command
    # runs under sh -e so a failed one fails the build, and the verifier then
    # requires all four records with exact counts, so it fails closed rather
    # than ship a boot partition the installer would refuse at first contact.
    CFGBYTES=\$(stat -c %s '$GRUB_CFG')
    : > /tmp/chains.txt
    for f in ::/boot/kernel ::/boot/fallback ::/boot/grub/grub.cfg \
             ::/boot/grub/grubenv; do
        mshowfat -i /tmp/p1.img \$f >> /tmp/chains.txt
    done
    python3 -c '
import sys, re
seen = []
sizes = {\"kernel\": $SLOT_BYTES, \"fallback\": int(sys.argv[1]),
         \"grub.cfg\": int(sys.argv[2]), \"grubenv\": 1024}
for line in open(\"/tmp/chains.txt\"):
    name = line.split()[0].rsplit(\"/\", 1)[-1]
    ranges = [(int(a), int(b)) for a, b in
              re.findall(r\"<(\\d+)-(\\d+)>\", line)]
    if not ranges:
        sys.exit(\"mkdisk_grub: %s has no clusters\" % name)
    n = sum(b - a + 1 for a, b in ranges)
    want = sizes.pop(name, None)
    if want is None:
        sys.exit(\"mkdisk_grub: unexpected or repeated record for %s\" % name)
    if n != (want + 511) // 512:
        sys.exit(\"mkdisk_grub: %s spans %d clusters and its %d bytes need %d\" %
                 (name, n, want, (want + 511) // 512))
    seen.append((name, ranges))
if sizes:
    sys.exit(\"mkdisk_grub: no chain record for %s\" %
             \", \".join(sorted(sizes)))
for i in range(len(seen)):
    for j in range(i + 1, len(seen)):
        for a1, b1 in seen[i][1]:
            for a2, b2 in seen[j][1]:
                if a1 <= b2 and a2 <= b1:
                    sys.exit(\"mkdisk_grub: %s and %s share clusters\" %
                             (seen[i][0], seen[j][0]))
print(\"mkdisk_grub: boot files verified:\",
      \" \".join(n for n, _ in seen))
' \"\$KBYTES\" \"\$CFGBYTES\"
    dd if=/tmp/p1.img of='$OUT.new' bs=1M seek=$GAP_MB conv=notrunc status=none

    grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' -o /tmp/core.img \
        biosdisk part_msdos fat normal multiboot configfile loadenv echo \
        test serial terminal
    python3 '$REPO/tools/grub_embed.py' '$OUT.new' \
        /usr/lib/grub/i386-pc/boot.img /tmp/core.img

    dd if='$KRFFS_BIN' of='$OUT.new' bs=1M seek=$((GAP_MB + BOOT_MB)) \
        conv=notrunc status=none
    mv '$OUT.new' '$OUT'
"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
