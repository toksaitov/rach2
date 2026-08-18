#!/usr/bin/env sh

#
# mkdisk_grub.sh: build a BIOS-bootable disk image with kernel slots.
#
# The disk has two MBR partitions, or three when a recovery partition is asked
# for: partition 1 is a FAT32 boot file system holding GRUB and two kernels,
# partition 2 is a KRFFS image, and partition 3, when RACH2_DISK_RECOVERY_MB is
# nonzero, is an empty FAT32 volume for the recovery kernel and its RAM image.
#
# GRUB's boot.img goes in the MBR and core.img in the post-MBR gap (partition 1
# starts at 1 MiB), so a real machine, or QEMU with no -kernel, boots
# BIOS -> MBR -> GRUB -> the Multiboot kernel, which then mounts KRFFS from
# partition 2 (rach2.root=disk).
#
# The boot partition is the install surface (dev-log/23-install/plan.md):
# /boot/kernel is the candidate, the kernel padded with zeros to a fixed
# 2 MiB slot whose data sectors bootctl rewrites in place on the
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

# Three more for the sealed disk, each defaulting to what this script always
# built. BOOT_MB is the FAT partition; CLUSTER is mformat's sectors-per-cluster
# (empty leaves mformat's own choice, which is one sector at the sizes this
# script used before); RECOVERY_MB adds a third partition after the root, and
# zero leaves the table with two entries as it has always had.
#
# The two are not independent. bootslot_open refuses a volume whose FAT exceeds
# BOOTSLOT_FAT_MAX_SECTORS (512) or whose cluster count exceeds
# BOOTSLOT_MAX_CLUSTERS (65536), and a 64 MiB partition at one sector per
# cluster is about 130500 clusters with a 1020-sector FAT, which fails both. At
# eight sectors per cluster it is 16348 clusters with a 128-sector FAT.
GAP_MB=1
BOOT_MB="${RACH2_DISK_BOOT_MB:-16}"
CLUSTER="${RACH2_DISK_CLUSTER:-}"
RECOVERY_MB="${RACH2_DISK_RECOVERY_MB:-0}"

# The FAT half of this disk used to differ between two builds of one tree, in
# two places that have nothing to do with its contents: mformat picks a random
# volume serial, and mcopy stamps each directory entry's creation and access
# dates from the clock. Both are pinned here. SOURCE_DATE_EPOCH is the standard
# name and mtools honours it; a caller that has one already keeps it, so this
# disk agrees with whatever else that caller is building.
#
# What is still not fixed, and is not pretended to be: the container is the
# floating ubuntu:24.04 tag and installs whatever GRUB and mtools the archive
# holds that day, and the machine's SSH host key and password salt are
# generated per tree. Those are inputs to the artifact, not noise in it.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export SOURCE_DATE_EPOCH
FAT_SERIAL=1acef00d

# The candidate slot: bootctl rewrites these bytes in place and never
# touches FAT metadata, so the file's size is fixed at build time.
SLOT_BYTES=2097152
# One spare MiB so the last partition ends before the device edge (parted
# rejects a partition that ends exactly at the device size).
DISK_MB=$((GAP_MB + BOOT_MB + KRFFS_MB + RECOVERY_MB + 1))

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
# candidate slot bootctl rewrites, /boot/fallback the untouched copy,
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
    export SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH
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
    if [ $RECOVERY_MB -gt 0 ]; then
        parted -s '$OUT.new' mkpart primary fat32 \
            $((GAP_MB + BOOT_MB + KRFFS_MB))MiB \
            $((GAP_MB + BOOT_MB + KRFFS_MB + RECOVERY_MB))MiB
    fi
    parted -s '$OUT.new' set 1 boot on

    KBYTES=\$(stat -c %s '$KERNEL')
    if [ \"\$KBYTES\" -gt $SLOT_BYTES ]; then
        echo \"mkdisk_grub: the kernel is \$KBYTES bytes and the slot holds $SLOT_BYTES\" >&2
        exit 1
    fi
    dd if=/dev/zero of=/tmp/candidate.pad bs=$SLOT_BYTES count=1 status=none
    dd if='$KERNEL' of=/tmp/candidate.pad conv=notrunc status=none
    dd if=/dev/zero of=/tmp/p1.img bs=1M count=$BOOT_MB status=none
    if [ -n '$CLUSTER' ]; then
        mformat -i /tmp/p1.img -F -N $FAT_SERIAL -c $CLUSTER ::
    else
        mformat -i /tmp/p1.img -F -N $FAT_SERIAL ::
    fi
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
# The cluster size comes from the volume that was just formatted, not from an
# assumption: this compared cluster counts against sector counts, which was
# only ever right because mformat happened to pick one sector per cluster.
bpb = open(\"/tmp/p1.img\", \"rb\").read(64)
cbytes = (bpb[11] | (bpb[12] << 8)) * bpb[13]
if cbytes <= 0:
    sys.exit(\"mkdisk_grub: the formatted volume reports a zero cluster size\")
for line in open(\"/tmp/chains.txt\"):
    name = line.split()[0].rsplit(\"/\", 1)[-1]
    # mshowfat prints a run as <a-b> and a lone cluster as <a>; a file small
    # enough to fit one cluster only appears in the second form, which a
    # larger cluster size makes the common case rather than the rare one.
    ranges = [(int(a), int(b if b else a)) for a, b in
              re.findall(r\"<(\\d+)(?:-(\\d+))?>\", line)]
    if not ranges:
        sys.exit(\"mkdisk_grub: %s has no clusters\" % name)
    n = sum(b - a + 1 for a, b in ranges)
    want = sizes.pop(name, None)
    if want is None:
        sys.exit(\"mkdisk_grub: unexpected or repeated record for %s\" % name)
    if n != (want + cbytes - 1) // cbytes:
        sys.exit(\"mkdisk_grub: %s spans %d clusters and its %d bytes need %d\" %
                 (name, n, want, (want + cbytes - 1) // cbytes))
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
print(\"mkdisk_grub: boot files verified at %d bytes per cluster:\" % cbytes,
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

    # Partition 3, when asked for: an empty FAT32 volume with the directories
    # its contents will live in. parted's filesystem argument only sets the MBR
    # type byte, so without this the entry would name a filesystem that is not
    # there, and GRUB could not read the partition the recovery entry needs.
    # What goes into it belongs to the phase that builds recovery.
    if [ $RECOVERY_MB -gt 0 ]; then
        dd if=/dev/zero of=/tmp/p3.img bs=1M count=$RECOVERY_MB status=none
        if [ -n '$CLUSTER' ]; then
            mformat -i /tmp/p3.img -F -N $FAT_SERIAL -c $CLUSTER ::
        else
            mformat -i /tmp/p3.img -F -N $FAT_SERIAL ::
        fi
        mmd -i /tmp/p3.img ::/boot
        mmd -i /tmp/p3.img ::/boot/grub
        dd if=/tmp/p3.img of='$OUT.new' bs=1M \
            seek=$((GAP_MB + BOOT_MB + KRFFS_MB)) conv=notrunc status=none
    fi
    mv '$OUT.new' '$OUT'
"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
