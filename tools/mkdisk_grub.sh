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
# capstones first (make).
#
# Usage: ./mkdisk_grub.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# The Ubuntu image, pinned by digest in tools/ubuntu-image.txt: a tag moves,
# and a GRUB built from a different grub-pc is a change nobody chose.
UBUNTU_IMAGE="$(sed -n 's/^\(ubuntu@sha256:[0-9a-f]*\)$/\1/p' \
                "$REPO/tools/ubuntu-image.txt" | head -1)"
[ -n "$UBUNTU_IMAGE" ] || { echo "$(basename "$0"): tools/ubuntu-image.txt has no digest" >&2; exit 1; }

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
# neither). The network tokens are separate from those, and in two sets: the
# ordinary entries take one and the static-address entries take the other,
# because the kernel refuses an address given twice, so a line carrying
# `rach2.ip=none` cannot have a fixed address appended to it.
MANIFEST="${RACH2_DISK_MANIFEST:-$REPO/krffs/manifests/os-real.manifest}"
KRFFS_MB="${RACH2_DISK_KRFFS_MB:-47}"
EXTRA_APPEND="${RACH2_DISK_APPEND:-}"
DHCP_APPEND="${RACH2_DISK_DHCP_APPEND:-}"
STATIC_APPEND="${RACH2_DISK_STATIC_APPEND:-}"

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
RECOVERY_KERNEL="${RACH2_DISK_RECOVERY_KERNEL:-}"
RECOVERY_IMAGE="${RACH2_DISK_RECOVERY_IMAGE:-}"

# The FAT half of this disk used to differ between two builds of one tree, in
# two places that have nothing to do with its contents: mformat picks a random
# volume serial, and mcopy stamps each directory entry's creation and access
# dates from the clock. Both are pinned here. SOURCE_DATE_EPOCH is the standard
# name and mtools honours it; a caller that has one already keeps it, so this
# disk agrees with whatever else that caller is building.
#
# What is still not fixed, and is not pretended to be: the container is the
# floating "$UBUNTU_IMAGE" tag and installs whatever GRUB and mtools the archive
# has that day, and the machine's SSH host key and password salt are
# generated per tree. Those are inputs to the artifact, not noise in it.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export SOURCE_DATE_EPOCH
FAT_SERIAL=1acef00d

# The candidate slot: bootctl rewrites these bytes in place and never
# touches FAT metadata, so the file's size is fixed at build time.
SLOT_BYTES=2097152
# The rewritable menu's slot. bootctl overwrites these bytes in place and never
# extends the file or moves its chain, so the size is fixed at build time and is
# a whole number of clusters at either geometry this script produces.
MENU_BYTES=8192
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
RECOVERY_CFG="$REPO/build/images/grub-recovery.cfg"
EMBED_CFG="$REPO/build/images/grub-embed.cfg"
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

# Every entry this file owns is defined before the rewritable half is read, and
# the order below is a contract. `source` merges the second file's entries onto
# the tail of this list, so these keep the low indices whatever that file has.
# Every entry names its own root. GRUB does not put `root` back when an entry
# fails: the recovery entries set it to partition 3, and a fallback that assumed
# the root it started with would look for the ordinary kernel there and fail
# too, which is a machine that stops instead of falling back.
#
# `set fallback` takes an entry NUMBER and an id does not resolve, so a menu
# supplied later cannot move what the fallback points at. A boot.cfg that is
# missing, truncated mid-write or wrong leaves these standing and the machine
# still boots.
#
#   0 candidate   1 fallback   2 candidate-static   3 fallback-static
#   4 debug       5 recovery   6 recovery-static

menuentry "Rach2" --id candidate {
    set root=(hd0,msdos1)
    multiboot /boot/kernel rach2.root=disk rach2.slot=candidate RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    boot
}

menuentry "Rach2 (fallback kernel)" --id fallback {
    set root=(hd0,msdos1)
    multiboot /boot/fallback rach2.root=disk rach2.slot=fallback RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    boot
}

menuentry "Rach2 (static address)" --id candidate-static {
    set root=(hd0,msdos1)
    multiboot /boot/kernel rach2.root=disk rach2.slot=candidate RACH2_APPEND_TOKENS RACH2_STATIC_TOKENS
    boot
}

menuentry "Rach2 (fallback kernel, static address)" --id fallback-static {
    set root=(hd0,msdos1)
    multiboot /boot/fallback rach2.root=disk rach2.slot=fallback RACH2_APPEND_TOKENS RACH2_STATIC_TOKENS
    boot
}

menuentry "Rach2 (kernel debug shell)" --id debug {
    set root=(hd0,msdos1)
    multiboot /boot/fallback rach2.root=disk rach2.slot=fallback rach2.rescue RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    boot
}

# The recovery kernel and its RAM image, on partition 3. It mounts nothing from
# this disk, which is what lets it rewrite the root; its title says so when the
# two files are there and says they are not when they are not, since the entry
# is defined either way (the numbers above are a contract and adding an entry
# afterwards would move them). There are two of it, for the same reason the
# ordinary entries come in pairs: a machine whose root will not mount has to be
# reachable, and on a network with no DHCP server the first one is not. Both
# carry `rach2.slot=recovery`, which is how `bootctl` running there knows it is
# running from neither kernel slot and may therefore install into either.
menuentry "RACH2_RECOVERY_TITLE" --id recovery {
    set root=(hd0,msdos3)
    multiboot /boot/recovery rach2.root=ram rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    module /boot/recovery.krffs
    boot
}

menuentry "RACH2_RECOVERY_STATIC_TITLE" --id recovery-static {
    set root=(hd0,msdos3)
    multiboot /boot/recovery rach2.root=ram rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_STATIC_TOKENS
    module /boot/recovery.krffs
    boot
}

# The rewritable half. `configfile` would open a new environment context, show
# its own menu on the spot and never come back here; `source` merges into this
# one and returns. The test keeps a missing file from being an error.
#
# This file is GRUB script, not a list of declarations, so whoever can write it
# can direct this boot: `source` will run a `set next_entry=` in it, or a
# `multiboot` and `boot` at its top level, before control comes back here. That
# is the same authority level one already grants, since `bootctl kernel`
# replaces the kernel that boots next. It is not what the entries above defend
# against.
#
# What they defend against is a file that is damaged, not written: a
# power cut during a write leaves a prefix of a new menu above the tail of an
# old one, and one slot overwritten in place cannot be written atomically. Every
# entry the machine needs is already defined by the time this line runs, so a
# `boot.cfg` that is missing, truncated, or unparseable costs only the entries
# it meant to add.
if [ -f /boot/boot.cfg ]; then
    source /boot/boot.cfg
fi

# The install contract (dev-log/23-install/plan.md): /boot/kernel is the
# candidate slot bootctl rewrites, /boot/fallback the untouched copy, and
# /boot/grub/grubenv the 1024-byte state block both GRUB and the installer edit
# in place. saved_entry and next_entry are compared against the exact menu ids
# and any other value selects the fallback entry, because the block has no
# checksum and a torn write can still load; next_entry is consumed here, the
# one-boot trial, since this GRUB does not consume it built-in.
# The fallback is set before the choices below, so the static entries can move
# it: GRUB reads this file top to bottom and the last assignment wins.
set default=fallback
set fallback=1
if [ "${saved_entry}" = "candidate" ]; then set default=candidate; fi
# A machine whose network has no DHCP server is left on a static entry, so the
# saved default takes those two as well; each also moves the fallback to the
# static one, since falling back from a fixed address to no address is the
# failure these entries exist to prevent.
if [ "${saved_entry}" = "candidate-static" ]; then
    set default=candidate-static
    set fallback=3
fi
if [ "${saved_entry}" = "fallback-static" ]; then
    set default=fallback-static
    set fallback=3
fi
# A repair is not one boot. Rewriting a root takes minutes, and a machine that
# lost power in the middle of one would come back to a kernel that cannot mount
# what is there and starts no way in; the boot partition is healthy, so nothing
# falls through to partition 3 either. So recovery can be the saved default as
# well as a one-boot trial, and an administrator leaves it there until the
# repair is finished. Each falls back to the kernel pair with its own network.
if [ "${saved_entry}" = "recovery" ]; then
    set default=recovery
fi
if [ "${saved_entry}" = "recovery-static" ]; then
    set default=recovery-static
    set fallback=3
fi
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
# A trial on a network with no DHCP server falls back to the kernel that also
# has the fixed address, entry 3. Falling back to entry 1 there is a machine
# that boots and cannot be reached, which is the one thing a fallback exists to
# prevent. The static recovery entry answers to the same rule: a recovery
# payload that will not load must not drop the machine onto a kernel that asks
# for a lease nobody answers.
if [ "${next_entry}" = "candidate-static" ]; then
    set default=candidate-static
    set fallback=3
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "fallback-static" ]; then
    set default=fallback-static
    set fallback=3
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "debug" ]; then
    set default=debug
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "recovery" ]; then
    set default=recovery
    set next_entry=
    save_env next_entry
fi
if [ "${next_entry}" = "recovery-static" ]; then
    set default=recovery-static
    set fallback=3
    set next_entry=
    save_env next_entry
fi

set timeout=3
CFG

# The per-machine tokens ride into the entries through three placeholders, so
# the heredoc above stays quoted and GRUB's own ${...} variables never reach the
# shell. The two network sets are what keeps the static entries honest: the
# ordinary entries take the DHCP set and the static ones take the fixed address,
# and neither line ever has both, since the kernel refuses an address given
# twice and would come up with no address at all.
RECOVERY_TITLE="Rach2 (recovery, not yet built)"
RECOVERY_STATIC_TITLE="Rach2 (recovery, static address, not yet built)"
if [ -n "$RECOVERY_KERNEL" ] && [ -n "$RECOVERY_IMAGE" ]; then
    RECOVERY_TITLE="Rach2 (recovery, rewrites this disk)"
    RECOVERY_STATIC_TITLE="Rach2 (recovery, static address)"
fi
sed -e "s|RACH2_STATIC_TOKENS|$STATIC_APPEND|" \
    -e "s|RACH2_DHCP_TOKENS|$DHCP_APPEND|" \
    -e "s|RACH2_RECOVERY_STATIC_TITLE|$RECOVERY_STATIC_TITLE|" \
    -e "s|RACH2_RECOVERY_TITLE|$RECOVERY_TITLE|" \
    -e "s|RACH2_APPEND_TOKENS|$EXTRA_APPEND|" "$GRUB_CFG" > "$GRUB_CFG.new"
mv "$GRUB_CFG.new" "$GRUB_CFG"

# The menu on partition 3. Everything above lives on partition 1, and a machine
# whose boot partition is damaged cannot read any of it: no menu, no entry, no
# recovery. What reaches this file is the configuration built into the core
# image, `tools/grub-embed.cfg`, which says there why it is shaped as it is;
# `configfile` does not retarget the root, which is why this file sets its own.
cat > "$RECOVERY_CFG" <<'RCFG'
set root=(hd0,msdos3)
set timeout=3
set default=0

menuentry "Rach2 (recovery, partition 1 unreadable)" --id recovery {
    multiboot /boot/recovery rach2.root=ram rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    module /boot/recovery.krffs
    boot
}

menuentry "Rach2 (recovery, partition 1 unreadable, static address)" --id recovery-static {
    multiboot /boot/recovery rach2.root=ram rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_STATIC_TOKENS
    module /boot/recovery.krffs
    boot
}
RCFG

sed -e "s|RACH2_STATIC_TOKENS|$STATIC_APPEND|" \
    -e "s|RACH2_DHCP_TOKENS|$DHCP_APPEND|" \
    -e "s|RACH2_APPEND_TOKENS|$EXTRA_APPEND|" "$RECOVERY_CFG" > "$RECOVERY_CFG.new"
mv "$RECOVERY_CFG.new" "$RECOVERY_CFG"

cp "$REPO/tools/grub-embed.cfg" "$EMBED_CFG"

# The lines whose absence is silent: a core that cannot reach partition 3 boots
# every ordinary machine and no damaged one, and one that reaches it first sends
# every healthy machine to recovery. Both builders check the same file with the
# same checker.
python3 "$REPO/tools/check_grubembed.py" "$EMBED_CFG"

# No entry may name the address twice. The kernel refuses a command line that
# does, and refuses it by coming up with no address at all, so a build that
# composed the two token sets wrongly would produce a disk whose menu offers a
# machine nobody can reach. It stops here instead.
if ! awk '/^[[:space:]]*multiboot /{ n = gsub(/rach2\.ip=/, "&"); if (n > 1) exit 1 }' \
        "$GRUB_CFG" "$RECOVERY_CFG"; then
    echo "mkdisk_grub: a menu entry names rach2.ip more than once" >&2
    exit 1
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
    "$UBUNTU_IMAGE" sh -e -c "
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
        echo \"mkdisk_grub: the kernel is \$KBYTES bytes and the slot has $SLOT_BYTES\" >&2
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
    # The rewritable menu, empty but full size. A '#' fill, not zeros
    # because GRUB reads the whole file and a comment is what an empty menu
    # looks like to it; the first line says so to anyone who reads the disk.
    dd if=/dev/zero bs=1 count=$MENU_BYTES status=none | tr '\\0' '#' > /tmp/boot.cfg
    printf '# no menu staged; bootctl config replaces this file\n' \
        | dd of=/tmp/boot.cfg conv=notrunc status=none
    mcopy -i /tmp/p1.img /tmp/boot.cfg ::/boot/boot.cfg
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
             ::/boot/grub/grubenv ::/boot/boot.cfg; do
        mshowfat -i /tmp/p1.img \$f >> /tmp/chains.txt
    done
    python3 -c '
import sys, re
seen = []
sizes = {\"kernel\": $SLOT_BYTES, \"fallback\": int(sys.argv[1]),
         \"grub.cfg\": int(sys.argv[2]), \"grubenv\": 1024,
         \"boot.cfg\": $MENU_BYTES}
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
    # larger cluster size makes the common case, not the rare one.
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
        -c '$EMBED_CFG' \
        biosdisk part_msdos fat normal multiboot configfile loadenv echo \
        test serial terminal
    python3 '$REPO/tools/grub_embed.py' '$OUT.new' \
        /usr/lib/grub/i386-pc/boot.img /tmp/core.img

    dd if='$KRFFS_BIN' of='$OUT.new' bs=1M seek=$((GAP_MB + BOOT_MB)) \
        conv=notrunc status=none

    # Partition 3, when asked for: a FAT32 volume holding the recovery kernel
    # and its RAM image, or the empty directories they will live in when this
    # build was not given them. parted's filesystem argument only sets the MBR
    # type byte, so without this the entry would name a filesystem that is not
    # there, and GRUB could not read the partition the recovery entry needs.
    if [ $RECOVERY_MB -gt 0 ]; then
        dd if=/dev/zero of=/tmp/p3.img bs=1M count=$RECOVERY_MB status=none
        if [ -n '$CLUSTER' ]; then
            mformat -i /tmp/p3.img -F -N $FAT_SERIAL -c $CLUSTER ::
        else
            mformat -i /tmp/p3.img -F -N $FAT_SERIAL ::
        fi
        mmd -i /tmp/p3.img ::/boot
        mmd -i /tmp/p3.img ::/boot/grub
        if [ -n '$RECOVERY_KERNEL' ]; then
            mcopy -i /tmp/p3.img '$RECOVERY_KERNEL' ::/boot/recovery
        fi
        if [ -n '$RECOVERY_IMAGE' ]; then
            mcopy -i /tmp/p3.img '$RECOVERY_IMAGE' ::/boot/recovery.krffs
        fi
        mcopy -i /tmp/p3.img '$RECOVERY_CFG' ::/boot/grub/recovery.cfg
        dd if=/tmp/p3.img of='$OUT.new' bs=1M \
            seek=$((GAP_MB + BOOT_MB + KRFFS_MB)) conv=notrunc status=none
    fi
    mv '$OUT.new' '$OUT'
"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
