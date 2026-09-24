#!/usr/bin/env sh

#
# mkdisk_grub.sh: build a BIOS-bootable disk image with kernel slots.
#
# The disk has one MBR partition, two when a recovery partition is asked for,
# and three when an ext2 root is. Partition 1 is a FAT32 boot file system
# holding GRUB and two kernel slots. Partition 2, when RACH2_DISK_RECOVERY_MB
# is nonzero, is a FAT32 volume for the recovery kernel and its RAM image, in a
# file system of its own so that a boot partition damaged by the writes an
# install makes still leaves a way to repair the machine. Partition 3, when
# RACH2_DISK_EXT2_MB is set, is the ext2 image RACH2_DISK_EXT2_IMAGE names,
# which both kernel entries boot as their root.
#
# There was a KRFFS root between them until 2026-09-14: a second complete
# system with the demonstration accounts, which the fallback entry booted so
# that a machine came up to something when the installed root would not mount.
# The recovery system does that and more, from RAM, and reaches the machine
# whether or not the boot partition reads; the fallback slot now guards the one
# failure GRUB can detect by itself, a kernel image that will not load, and
# boots the person's own root with it.
#
# GRUB's boot.img goes in the MBR and core.img in the post-MBR gap (partition 1
# starts at 1 MiB), so a real machine, or QEMU with no -kernel, boots
# BIOS -> MBR -> GRUB -> the Multiboot kernel.
#
# The boot partition is the install surface (dev-log/23-install/plan.md):
# /boot/kernel is the candidate, the kernel padded with zeros to a fixed
# 2 MiB slot whose data sectors bootctl rewrites in place on the
# machine itself; /boot/fallback is the same kernel, padded the same, which
# `bootctl fallback` rewrites from a committed candidate and nothing else;
# and /boot/grub/grubenv is the 1024-byte state block that selects between
# them, initialized to boot the candidate. The build verifies every staged
# chain and the four files' disjointness before committing the partition,
# because the installer walks the same structures and a broken chain would
# stop every install.
#
# The disk is assembled on the host with mtools, the project MBR writer, and
# the pinned GRUB artifacts under vendor/grub/, so no GRUB toolchain and no
# Docker is needed.
# Build the kernel and the capstones first (make).
#
# Usage: ./mkdisk_grub.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

# The kernel and output default to the host build's, and both can be named:
# the self-build proof hands the guest-built kernel here and boots the result.
# The output's default is the name the root Makefile gives the GRUB disk, with
# `-with-key` in it when this tree has a recovery key.
KERNEL="${1:-$REPO/build/rach2/kernel}"
KEY_TAG=""
[ ! -f "$REPO/krffs/recovery_authorized_keys" ] || KEY_TAG=-with-key
OUT="${2:-$REPO/build/images/os-real-grub$KEY_TAG.disk}"

# Knobs for a disk aimed at a particular machine, each defaulting to exactly
# what this script always built so no existing caller changes: extra kernel
# command-line tokens appended to every menu entry (a real
# machine needs its address and its disk driver named; QEMU's defaults need
# neither). The network tokens are separate from those, since the kernel
# refuses an address given twice and a line carrying `rach2.ip=none` can
# have no address appended to it.
EXTRA_APPEND="${RACH2_DISK_APPEND:-}"
DHCP_APPEND="${RACH2_DISK_DHCP_APPEND:-}"

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
# A file to read in each FAT partition, so that every partition of a fresh
# disk has one and a person can tell which is which by reading it.
HELLO="${RACH2_DISK_HELLO:-}"
RECOVERY_KERNEL="${RACH2_DISK_RECOVERY_KERNEL:-}"
RECOVERY_IMAGE="${RACH2_DISK_RECOVERY_IMAGE:-}"
# The third partition: the ext2 root image, at most EXT2_MB in size, which
# both kernel entries boot.
EXT2_MB="${RACH2_DISK_EXT2_MB:-0}"
EXT2_IMAGE="${RACH2_DISK_EXT2_IMAGE:-}"
# Every disk this builds has a root, because both kernel entries name one and a
# disk without it boots to nothing. The KRFFS root that used to stand in for it
# is gone, and the callers that relied on it were found by this check rather
# than by booting the disks they produced.
if [ "$EXT2_MB" -le 0 ]; then
    echo "mkdisk_grub: this disk would have no root; set RACH2_DISK_EXT2_MB and RACH2_DISK_EXT2_IMAGE" >&2
    exit 1
fi
if [ "$EXT2_MB" -gt 0 ] && [ "$RECOVERY_MB" -le 0 ]; then
    echo "mkdisk_grub: the ext2 partition is the third; RACH2_DISK_RECOVERY_MB must be set with it" >&2
    exit 1
fi
if [ "$EXT2_MB" -gt 0 ] && [ ! -f "$EXT2_IMAGE" ]; then
    echo "mkdisk_grub: RACH2_DISK_EXT2_IMAGE names no file ($EXT2_IMAGE)" >&2
    exit 1
fi
# The identities: the MBR's disk id, which the menu names the roots and the
# boot partition by, and the two FAT serials, which the menu finds the
# partitions holding the kernels by. Pinned here, distinct from each other
# so a search by serial finds one partition, and replaced by the installer
# when it copies a disk. The labels are what the core image searches for
# and are the same on every disk.
MBR_ID=1acef00d
FAT_SERIAL=1acef00d
RECOVERY_SERIAL=1acef003
BOOT_LABEL=RACH2BOOT
RECOVERY_LABEL=RACH2RECOV
# The root by identity: the disk id and the entry, with the type beside it, so
# a clone of this disk behind it is not taken for it and a partition of the
# wrong kind is refused by name.
#
# Both kernel entries take the same root, the ext2 one. They used to differ:
# the fallback booted a KRFFS root of its own in a partition of its own, a
# second complete system with the demonstration accounts, kept so a machine
# came up to something when the installed root would not mount. The recovery
# system does that job and does it better, from RAM, with the tools to repair
# the thing, and it is reached whether or not the boot partition reads. So the
# fallback slot now guards the one failure it can actually catch, a kernel
# image that will not load: GRUB falls into it by itself and the machine comes
# back on the previous kernel with the person's own files.
CANDIDATE_ROOT="MBRID=$MBR_ID,3 rach2.rootfstype=ext2"
FALLBACK_ROOT="MBRID=$MBR_ID,3 rach2.rootfstype=ext2"

# The FAT half of this disk used to differ between two builds of one tree, in
# two places that have nothing to do with its contents: mformat picks a random
# volume serial, and mcopy stamps each directory entry's creation and access
# dates from the clock. Both are pinned here. SOURCE_DATE_EPOCH is the standard
# name and mtools honours it; a caller that has one already keeps it, so this
# disk agrees with whatever else that caller is building.
#
# What is still not fixed, and is not pretended to be: the machine's SSH
# host key and password salt are generated per tree. Those are inputs to
# the artifact, not noise in it. The GRUB bytes themselves are pinned under
# vendor/grub/ and verified before use.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export SOURCE_DATE_EPOCH

# The candidate slot: bootctl rewrites these bytes in place and never
# touches FAT metadata, so the file's size is fixed at build time.
SLOT_BYTES=2097152
# The rewritable menu's slot. bootctl overwrites these bytes in place and never
# extends the file or moves its chain, so the size is fixed at build time and is
# a whole number of clusters at either geometry this script produces.
MENU_BYTES=8192
# One spare MiB so the last partition ends before the device edge (parted
# rejects a partition that ends exactly at the device size).
# The ext2 partition begins after the spare MiB, where the installed machine's
# fourth partition already begins, so the sealed image and the machine agree
# on its offset; a second spare MiB then ends the disk.
EXT2_START_MB=$((GAP_MB + BOOT_MB + RECOVERY_MB + 1))
DISK_MB=$((EXT2_START_MB + EXT2_MB + 1))
if [ "$EXT2_MB" -le 0 ]; then
    DISK_MB=$((GAP_MB + BOOT_MB + RECOVERY_MB + 1))
fi

if [ ! -f "$KERNEL" ]; then
    echo "mkdisk_grub: kernel not built ($KERNEL); run 'make' first" >&2
    exit 1
fi
for t in mformat mmd mcopy; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "mkdisk_grub: mtools is required (brew install mtools, or apt install mtools)" >&2
        exit 1
    }
done

# The MBR writer beside this script, built by make when missing or older
# than its source; it writes the same bytes parted wrote in the retired
# container, CHS fields included. The chain reader and the GRUB embedder are
# built the same way.
MKMBR="$REPO/build/tools/image/mkmbr"
make -s -C "$REPO" build/tools/image/mkmbr || exit 1
FATCHAINS="$REPO/build/tools/image/fatchains"
make -s -C "$REPO" build/tools/image/fatchains || exit 1
GRUBEMBED="$REPO/build/tools/image/grubembed"
make -s -C "$REPO" build/tools/image/grubembed || exit 1

# Every intermediate lives in this run's own directory, so two profiles
# building in parallel (the suites do) cannot overwrite each other's
# configuration or partition images. The trap clears it and the unrenamed
# output on any exit; the finished disk appears only through the final mv.
TMPD="$(mktemp -d "$REPO/build/images/mkdisk.XXXXXX")"
# The unrenamed output carries this run's process number for the same reason:
# two builds of one disk with one fixed name removed each other's file, and
# make deleted the disk both wanted. On 2026-09-09 the installer and
# installer-repair suites went down that way, six and twelve seconds each.
NEW="$OUT.$$.new"
trap 'rm -rf "$TMPD" "$NEW"' EXIT
GRUB_CFG="$TMPD/grub-disk.cfg"
RECOVERY_CFG="$TMPD/grub-recovery.cfg"
EMBED_CFG="$TMPD/grub-embed.cfg"

# The pinned GRUB artifacts: copied into this run's directory first, then
# the copies verified against the manifest, so the bytes checked are the
# bytes written to the disk and a regeneration racing this build cannot
# slip a file between the check and the read.
cp "$REPO/vendor/grub/bios-boot.img" "$TMPD/bios-boot.img"
cp "$REPO/vendor/grub/disk-core.img" "$TMPD/disk-core.img"
sh "$REPO/vendor/grub/verify_grub.sh" \
    "bios-boot.img=$TMPD/bios-boot.img" \
    "disk-core.img=$TMPD/disk-core.img"

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
# Every entry finds its own root, by the serial of the partition holding its
# kernel, wherever the firmware put this disk. GRUB does not put `root` back
# when an entry fails: the recovery entries set it to partition 3, and a
# fallback that assumed the root it started with would look for the ordinary
# kernel there and fail too, which is a machine that stops instead of falling
# back.
#
# `set fallback` takes an entry NUMBER and an id does not resolve, so a menu
# supplied later cannot move what the fallback points at. A boot.cfg that is
# missing, truncated mid-write or wrong leaves these standing and the machine
# still boots.
#
#   0 candidate   1 fallback   2 recovery

menuentry "Start Rach2" --id candidate {
    search --no-floppy --fs-uuid --set=root RACH2_BOOT_SERIAL
    multiboot /boot/kernel rach2.root=RACH2_CANDIDATE_ROOT rach2.boot=RACH2_BOOT_PART rach2.slot=candidate RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    boot
}

menuentry "Start Rach2 with the previous kernel" --id fallback {
    search --no-floppy --fs-uuid --set=root RACH2_BOOT_SERIAL
    multiboot /boot/fallback rach2.root=RACH2_FALLBACK_ROOT rach2.boot=RACH2_BOOT_PART rach2.slot=fallback RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    boot
}




# The recovery kernel and its RAM image, on partition 2. It mounts nothing from
# this disk, which is what lets it rewrite the root; its title says so when the
# two files are there and says they are not when they are not, since the entry
# is defined either way (the numbers above are a contract and adding an entry
# afterwards would move them). `/boot/grub/recovery.cfg` on the recovery
# partition, written from the heredoc below, has a second one, which the
# core image's embedded configuration loads when the boot partition does
# not read. Both carry
# `rach2.slot=recovery`, which is how `bootctl` running there knows it is
# running from neither kernel slot and may therefore install into either.
menuentry "RACH2_RECOVERY_TITLE" --id recovery {
    search --no-floppy --fs-uuid --set=root RACH2_RECOVERY_SERIAL
    multiboot /boot/recovery rach2.root=ram rach2.boot=RACH2_BOOT_PART rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    module /boot/recovery.krffs
    boot
}


# Recovery changes nothing a person did not ask for: its line carries no
# `rach2.fsck`, so nothing is checked or repaired by the act of booting it.
# `e2fsck` is there, and a person who came to repair a root runs it. A second
# recovery entry that ran the check at boot was removed on 2026-09-14, with the
# two entries that carried a fixed address: a menu of eight entries asked a
# person at a broken machine to choose between things only its author could
# tell apart. `bootctl trial recovery` arms recovery for one boot, which is how
# a machine that still starts is sent to the image that can rewrite its root.

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
# Entry 1 is the previous kernel, and GRUB takes a number here and not an id:
# `get_and_remove_first_entry_number` parses this and discards anything that is
# not one, so a name would leave the machine with no automatic fallback at all.
# The order of the entries above is therefore part of this file's contract.
set default=fallback
set fallback=1
if [ "${saved_entry}" = "candidate" ]; then set default=candidate; fi
# A repair is not one boot. Rewriting a root takes minutes, and a machine that
# lost power in the middle of one would come back to a kernel that cannot mount
# what is there and starts no way in; the boot partition is healthy, so nothing
# falls through to the kernel entries either. So recovery can be the saved
# default as well as a one-boot trial, and an administrator leaves it there
# until the repair is finished.
if [ "${saved_entry}" = "recovery" ]; then
    set default=recovery
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
if [ "${next_entry}" = "recovery" ]; then
    set default=recovery
    set next_entry=
    save_env next_entry
fi

set timeout=3
CFG

# The script substitutes the per-machine tokens into the entries through
# two placeholders, so the heredoc above stays quoted and GRUB's own ${...}
# variables never reach the shell. The network tokens are a set of their
# own, since the kernel refuses an address given twice and would come up
# with no address
# at all. The values are escaped before sed sees
# them, because `&`, `\`, and the delimiter are replacement syntax and a
# token containing one would silently change; a newline would end the sed
# expression, so it is refused outright.
for v in "$EXTRA_APPEND" "$DHCP_APPEND"; do
    if [ "$(printf '%s' "$v" | wc -l)" -gt 0 ]; then
        echo "mkdisk_grub: an append token set contains a newline" >&2
        exit 1
    fi
done
sedq() { printf '%s' "$1" | sed 's/[\\&|]/\\&/g'; }
# A FAT serial as GRUB's `search --fs-uuid` names it: XXXX-XXXX, upper case.
fat_uuid() { printf '%s' "$1" | tr 'a-f' 'A-F' | sed 's/^\(....\)\(....\)$/\1-\2/'; }
RECOVERY_TITLE="Start the Rach2 recovery system (not yet built)"
if [ -n "$RECOVERY_KERNEL" ] && [ -n "$RECOVERY_IMAGE" ]; then
    RECOVERY_TITLE="Start the Rach2 recovery system"
fi
sed -e "s|RACH2_DHCP_TOKENS|$(sedq "$DHCP_APPEND")|" \
    -e "s|RACH2_CANDIDATE_ROOT|$CANDIDATE_ROOT|" \
    -e "s|RACH2_FALLBACK_ROOT|$FALLBACK_ROOT|" \
    -e "s|RACH2_BOOT_PART|MBRID=$MBR_ID,1|" \
    -e "s|RACH2_BOOT_SERIAL|$(fat_uuid $FAT_SERIAL)|" \
    -e "s|RACH2_RECOVERY_SERIAL|$(fat_uuid $RECOVERY_SERIAL)|" \
    -e "s|RACH2_RECOVERY_TITLE|$(sedq "$RECOVERY_TITLE")|" \
    -e "s|RACH2_APPEND_TOKENS|$(sedq "$EXTRA_APPEND")|" \
    "$GRUB_CFG" > "$GRUB_CFG.new"
mv "$GRUB_CFG.new" "$GRUB_CFG"

# The menu on partition 3. Everything above lives on partition 1, and a machine
# whose boot partition is damaged cannot read any of it: no menu, no entry, no
# recovery. What reaches this file is the configuration built into the core
# image, `grub-embed.cfg` beside this script, which says why it is shaped so;
# `configfile` does not retarget the root, which is why this file sets its own.
cat > "$RECOVERY_CFG" <<'RCFG'
search --no-floppy --fs-uuid --set=root RACH2_RECOVERY_SERIAL
set timeout=3
set default=0

menuentry "Start the Rach2 recovery system (the boot partition does not read)" --id recovery {
    multiboot /boot/recovery rach2.root=ram rach2.boot=RACH2_BOOT_PART rach2.slot=recovery RACH2_APPEND_TOKENS RACH2_DHCP_TOKENS
    module /boot/recovery.krffs
    boot
}

RCFG

sed -e "s|RACH2_DHCP_TOKENS|$(sedq "$DHCP_APPEND")|" \
    -e "s|RACH2_BOOT_PART|MBRID=$MBR_ID,1|" \
    -e "s|RACH2_RECOVERY_SERIAL|$(fat_uuid $RECOVERY_SERIAL)|" \
    -e "s|RACH2_APPEND_TOKENS|$(sedq "$EXTRA_APPEND")|" \
    "$RECOVERY_CFG" > "$RECOVERY_CFG.new"
mv "$RECOVERY_CFG.new" "$RECOVERY_CFG"

cp "$REPO/tools/image/grub-embed.cfg" "$EMBED_CFG"

# The lines whose absence is silent: a core that cannot reach partition 3 boots
# every ordinary machine and no damaged one, and one that reaches it first sends
# every healthy machine to recovery. Both builders check the same file with the
# same checker.
CFGCHECK="$REPO/build/tools/inspect/cfgcheck"
if [ ! -x "$CFGCHECK" ] || [ "$REPO/tools/inspect/cfgcheck.c" -nt "$CFGCHECK" ]; then
    mkdir -p "${CFGCHECK%/*}"
    cc -O2 -o "$CFGCHECK" "$REPO/tools/inspect/cfgcheck.c" || exit 1
fi
"$CFGCHECK" grubembed "$EMBED_CFG"

# No entry may name the address twice. The kernel refuses a command line that
# does, and refuses it by coming up with no address at all, so a build that
# composed the two token sets wrongly would produce a disk whose menu offers a
# machine nobody can reach. It stops here instead.
if ! awk '/^[[:space:]]*multiboot /{ n = gsub(/rach2\.ip=/, "&"); if (n > 1) exit 1 }' \
        "$GRUB_CFG" "$RECOVERY_CFG"; then
    echo "mkdisk_grub: a menu entry names rach2.ip more than once" >&2
    exit 1
fi

# Everything below runs on the host: mtools for the FAT volumes, the MBR
# writer for the partition table, the pinned GRUB core for the boot path.
# No container, no loop mount, no GRUB toolchain.
export MTOOLS_SKIP_CHECK=1
# UTC, so the SOURCE_DATE_EPOCH timestamps land in FAT fields the way the
# retired container (which ran UTC) wrote them.
export TZ=UTC

dd if=/dev/zero of="$NEW" bs=1048576 count="$DISK_MB" 2>/dev/null
if [ "$EXT2_MB" -gt 0 ]; then
    "$MKMBR" --id "$MBR_ID" "$NEW" "${GAP_MB}:${BOOT_MB}:0c:boot" \
        "$((GAP_MB + BOOT_MB)):${RECOVERY_MB}:0c" \
        "${EXT2_START_MB}:${EXT2_MB}:83"
elif [ "$RECOVERY_MB" -gt 0 ]; then
    "$MKMBR" --id "$MBR_ID" "$NEW" "${GAP_MB}:${BOOT_MB}:0c:boot" \
        "$((GAP_MB + BOOT_MB)):${RECOVERY_MB}:0c"
else
    "$MKMBR" --id "$MBR_ID" "$NEW" "${GAP_MB}:${BOOT_MB}:0c:boot"
fi

KBYTES=$(wc -c < "$KERNEL" | tr -d ' ')
if [ "$KBYTES" -gt "$SLOT_BYTES" ]; then
    echo "mkdisk_grub: the kernel is $KBYTES bytes and the slot has $SLOT_BYTES" >&2
    exit 1
fi

dd if=/dev/zero of="$TMPD/candidate.pad" bs="$SLOT_BYTES" count=1 2>/dev/null
dd if="$KERNEL" of="$TMPD/candidate.pad" conv=notrunc 2>/dev/null
dd if=/dev/zero of="$TMPD/p1.img" bs=1048576 count="$BOOT_MB" 2>/dev/null
if [ -n "$CLUSTER" ]; then
    mformat -i "$TMPD/p1.img" -F -N $FAT_SERIAL -v $BOOT_LABEL -c "$CLUSTER" ::
else
    mformat -i "$TMPD/p1.img" -F -N $FAT_SERIAL -v $BOOT_LABEL ::
fi
mmd -i "$TMPD/p1.img" ::/boot
mmd -i "$TMPD/p1.img" ::/boot/grub
mcopy -i "$TMPD/p1.img" "$TMPD/candidate.pad" ::/boot/kernel
mcopy -i "$TMPD/p1.img" "$TMPD/candidate.pad" ::/boot/fallback
mcopy -i "$TMPD/p1.img" "$GRUB_CFG" ::/boot/grub/grub.cfg
# The rewritable menu, empty but full size. A '#' fill, not zeros, because
# GRUB reads the whole file and a comment is what an empty menu looks like
# to it; the first line says so to anyone who reads the disk.
dd if=/dev/zero bs=1 count="$MENU_BYTES" 2>/dev/null | tr '\0' '#' > "$TMPD/boot.cfg"
printf '# no menu staged; bootctl config replaces this file\n' \
    | dd of="$TMPD/boot.cfg" conv=notrunc 2>/dev/null
mcopy -i "$TMPD/p1.img" "$TMPD/boot.cfg" ::/boot/boot.cfg
# The 1024-byte GRUB environment block, written here byte-for-byte the way
# grub-editenv wrote it in the retired container (a header, one variable,
# and a '#' fill), proved identical by digest when the container path was
# converted.
{
    printf '# GRUB Environment Block\n'
    printf '# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
    printf 'saved_entry=candidate\n'
} > "$TMPD/genv.head"
GENVN=$(wc -c < "$TMPD/genv.head" | tr -d ' ')
{
    cat "$TMPD/genv.head"
    dd if=/dev/zero bs=1 count=$((1024 - GENVN)) 2>/dev/null | tr '\0' '#'
} > "$TMPD/genv"
mcopy -i "$TMPD/p1.img" "$TMPD/genv" ::/boot/grub/grubenv
if [ -n "$HELLO" ]; then
    mcopy -i "$TMPD/p1.img" "$HELLO" ::/hello.txt
fi

# Every staged chain, verified before the partition is committed: each of
# the five files must be whole (its cluster count matching its size) and
# the extents disjoint. fatchains prints cluster ranges, the BPB names the
# cluster size, and the awk below fails closed: a missing record, a wrong
# count, or a shared cluster stops the build.
CFGBYTES=$(wc -c < "$GRUB_CFG" | tr -d ' ')
"$FATCHAINS" "$TMPD/p1.img" ::/boot/kernel ::/boot/fallback ::/boot/grub/grub.cfg \
    ::/boot/grub/grubenv ::/boot/boot.cfg > "$TMPD/chains.txt" || exit 1
# shellcheck disable=SC2046 # od output is exactly three numeric fields
set -- $(od -An -j 11 -N 3 -t u1 "$TMPD/p1.img")
CLBYTES=$(( ($1 + $2 * 256) * $3 ))
[ "$CLBYTES" -gt 0 ] || {
    echo "mkdisk_grub: the formatted volume reports a zero cluster size" >&2
    exit 1
}
awk -v cbytes="$CLBYTES" -v kernel="$SLOT_BYTES" -v fallback="$SLOT_BYTES" \
    -v grubcfg="$CFGBYTES" -v grubenv=1024 -v bootcfg="$MENU_BYTES" '
function need(bytes) { return int((bytes + cbytes - 1) / cbytes) }
{
    file = $1
    sub(/.*\//, "", file)
    want = file == "kernel" ? kernel : file == "fallback" ? fallback : \
           file == "grub.cfg" ? grubcfg : file == "grubenv" ? grubenv : \
           file == "boot.cfg" ? bootcfg : -1
    if (want < 0) {
        print "mkdisk_grub: unexpected record for " file > "/dev/stderr"
        bad = 1; exit 1
    }
    if (file in seen) {
        print "mkdisk_grub: repeated record for " file > "/dev/stderr"
        bad = 1; exit 1
    }
    seen[file] = 1
    count = 0
    rest = $0
    while (match(rest, /<[0-9]+(-[0-9]+)?>/)) {
        run = substr(rest, RSTART + 1, RLENGTH - 2)
        rest = substr(rest, RSTART + RLENGTH)
        dash = index(run, "-")
        a = dash ? substr(run, 1, dash - 1) + 0 : run + 0
        b = dash ? substr(run, dash + 1) + 0 : run + 0
        count += b - a + 1
        for (c = a; c <= b; c++) {
            if (c in used) {
                print "mkdisk_grub: " file " and " used[c] " share clusters" \
                    > "/dev/stderr"
                bad = 1; exit 1
            }
            used[c] = file
        }
    }
    if (count == 0) {
        print "mkdisk_grub: " file " has no clusters" > "/dev/stderr"
        bad = 1; exit 1
    }
    if (count != need(want)) {
        print "mkdisk_grub: " file " spans " count " clusters and its " \
              want " bytes need " need(want) > "/dev/stderr"
        bad = 1; exit 1
    }
}
END {
    # exit in a rule still runs END, so the success line is printed only
    # when no rule flagged a failure first.
    if (bad) exit 1
    if (!("kernel" in seen && "fallback" in seen && "grub.cfg" in seen && \
          "grubenv" in seen && "boot.cfg" in seen)) {
        print "mkdisk_grub: a chain record is missing" > "/dev/stderr"
        exit 1
    }
    printf "mkdisk_grub: boot files verified at %d bytes per cluster\n", cbytes
}' "$TMPD/chains.txt"

dd if="$TMPD/p1.img" of="$NEW" bs=1048576 seek="$GAP_MB" conv=notrunc 2>/dev/null

"$GRUBEMBED" "$NEW" "$TMPD/bios-boot.img" "$TMPD/disk-core.img"


# Partition 2, when asked for: a FAT32 volume holding the recovery kernel
# and its RAM image, or the empty directories they will live in when this
# build was not given them.
if [ "$RECOVERY_MB" -gt 0 ]; then
    dd if=/dev/zero of="$TMPD/p3.img" bs=1048576 count="$RECOVERY_MB" 2>/dev/null
    if [ -n "$CLUSTER" ]; then
        mformat -i "$TMPD/p3.img" -F -N $RECOVERY_SERIAL -v $RECOVERY_LABEL -c "$CLUSTER" ::
    else
        mformat -i "$TMPD/p3.img" -F -N $RECOVERY_SERIAL -v $RECOVERY_LABEL ::
    fi
    mmd -i "$TMPD/p3.img" ::/boot
    mmd -i "$TMPD/p3.img" ::/boot/grub
    if [ -n "$RECOVERY_KERNEL" ]; then
        mcopy -i "$TMPD/p3.img" "$RECOVERY_KERNEL" ::/boot/recovery
    fi
    if [ -n "$RECOVERY_IMAGE" ]; then
        mcopy -i "$TMPD/p3.img" "$RECOVERY_IMAGE" ::/boot/recovery.krffs
    fi
    mcopy -i "$TMPD/p3.img" "$RECOVERY_CFG" ::/boot/grub/recovery.cfg
    if [ -n "$HELLO" ]; then
        mcopy -i "$TMPD/p3.img" "$HELLO" ::/hello.txt
    fi
    dd if="$TMPD/p3.img" of="$NEW" bs=1048576 \
        seek=$((GAP_MB + BOOT_MB)) conv=notrunc 2>/dev/null
fi
# Partition 3, when asked for: the ext2 root image, which must fit.
if [ "$EXT2_MB" -gt 0 ]; then
    EXT2_BYTES=$(wc -c < "$EXT2_IMAGE" | tr -d ' ')
    if [ "$EXT2_BYTES" -gt $((EXT2_MB * 1048576)) ]; then
        echo "mkdisk_grub: the ext2 image is $EXT2_BYTES bytes and partition 3 has $((EXT2_MB * 1048576))" >&2
        rm -f "$NEW"
        exit 1
    fi
    dd if="$EXT2_IMAGE" of="$NEW" bs=1048576 \
        seek="$EXT2_START_MB" conv=notrunc 2>/dev/null
fi
rm -rf "$TMPD"
mv "$NEW" "$OUT"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
