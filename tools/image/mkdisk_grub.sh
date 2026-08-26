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
# The disk is assembled on the host with mtools, the project MBR writer, and
# the pinned GRUB artifacts under vendor/grub/, so no GRUB toolchain and no
# Docker is needed. The KRFFS partition image is built first with mkimage.sh.
# Build the kernel and the capstones first (make).
#
# Usage: ./mkdisk_grub.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

# The kernel and output default to the host build's, and both can be named:
# the self-build proof hands the guest-built kernel here and boots the result.
KERNEL="${1:-$REPO/build/rach2/kernel}"
OUT="${2:-$REPO/build/images/os-real-grub.disk}"

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
# What is still not fixed, and is not pretended to be: the machine's SSH
# host key and password salt are generated per tree. Those are inputs to
# the artifact, not noise in it. The GRUB bytes themselves are pinned under
# vendor/grub/ and verified before use.
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
for t in mformat mmd mcopy mshowfat; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "mkdisk_grub: mtools is required (brew install mtools, or apt install mtools)" >&2
        exit 1
    }
done

# The MBR writer beside this script, compiled when missing or older than
# its source; it writes the same bytes parted wrote in the retired
# container, CHS fields included.
MKMBR="$REPO/build/tools/image/mkmbr"
if [ ! -x "$MKMBR" ] || [ "$REPO/tools/image/mkmbr.c" -nt "$MKMBR" ]; then
    mkdir -p "${MKMBR%/*}"
    cc -O2 -o "$MKMBR" "$REPO/tools/image/mkmbr.c" || exit 1
fi

# Every intermediate lives in this run's own directory, so two profiles
# building in parallel (the suites do) cannot overwrite each other's
# configuration or partition images. The trap clears it and the unrenamed
# output on any exit; the finished disk appears only through the final mv.
TMPD="$(mktemp -d "$REPO/build/images/mkdisk.XXXXXX")"
trap 'rm -rf "$TMPD" "$OUT.new"' EXIT
KRFFS_BIN="$TMPD/grub-krffs.bin"
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

# The KRFFS partition image, sized to partition 2, built with the host tools.
sh "$REPO/krffs/mkimage.sh" "$MANIFEST" \
    "$KRFFS_BIN" "$KRFFS_MB"

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

# The per-machine tokens ride into the entries through three placeholders,
# so the heredoc above stays quoted and GRUB's own ${...} variables never
# reach the shell. Two separate network sets: the ordinary entries take the
# DHCP set and the static ones take the fixed address, and neither line
# ever has both, since the kernel refuses an address given twice and would
# come up with no address at all. The values are escaped before sed sees
# them, because `&`, `\`, and the delimiter are replacement syntax and a
# token containing one would silently change; a newline would end the sed
# expression, so it is refused outright.
for v in "$EXTRA_APPEND" "$DHCP_APPEND" "$STATIC_APPEND"; do
    if [ "$(printf '%s' "$v" | wc -l)" -gt 0 ]; then
        echo "mkdisk_grub: an append token set contains a newline" >&2
        exit 1
    fi
done
sedq() { printf '%s' "$1" | sed 's/[\\&|]/\\&/g'; }
RECOVERY_TITLE="Rach2 (recovery, not yet built)"
RECOVERY_STATIC_TITLE="Rach2 (recovery, static address, not yet built)"
if [ -n "$RECOVERY_KERNEL" ] && [ -n "$RECOVERY_IMAGE" ]; then
    RECOVERY_TITLE="Rach2 (recovery, rewrites this disk)"
    RECOVERY_STATIC_TITLE="Rach2 (recovery, static address)"
fi
sed -e "s|RACH2_STATIC_TOKENS|$(sedq "$STATIC_APPEND")|" \
    -e "s|RACH2_DHCP_TOKENS|$(sedq "$DHCP_APPEND")|" \
    -e "s|RACH2_RECOVERY_STATIC_TITLE|$(sedq "$RECOVERY_STATIC_TITLE")|" \
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

sed -e "s|RACH2_STATIC_TOKENS|$(sedq "$STATIC_APPEND")|" \
    -e "s|RACH2_DHCP_TOKENS|$(sedq "$DHCP_APPEND")|" \
    -e "s|RACH2_APPEND_TOKENS|$(sedq "$EXTRA_APPEND")|" \
    "$RECOVERY_CFG" > "$RECOVERY_CFG.new"
mv "$RECOVERY_CFG.new" "$RECOVERY_CFG"

cp "$REPO/tools/image/grub-embed.cfg" "$EMBED_CFG"

# The lines whose absence is silent: a core that cannot reach partition 3 boots
# every ordinary machine and no damaged one, and one that reaches it first sends
# every healthy machine to recovery. Both builders check the same file with the
# same checker.
python3 "$REPO/tools/inspect/check_grubembed.py" "$EMBED_CFG"

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

dd if=/dev/zero of="$OUT.new" bs=1048576 count="$DISK_MB" 2>/dev/null
if [ "$RECOVERY_MB" -gt 0 ]; then
    "$MKMBR" "$OUT.new" "${GAP_MB}:${BOOT_MB}:0c:boot" \
        "$((GAP_MB + BOOT_MB)):${KRFFS_MB}:83" \
        "$((GAP_MB + BOOT_MB + KRFFS_MB)):${RECOVERY_MB}:0c"
else
    "$MKMBR" "$OUT.new" "${GAP_MB}:${BOOT_MB}:0c:boot" \
        "$((GAP_MB + BOOT_MB)):${KRFFS_MB}:83"
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
    mformat -i "$TMPD/p1.img" -F -N $FAT_SERIAL -c "$CLUSTER" ::
else
    mformat -i "$TMPD/p1.img" -F -N $FAT_SERIAL ::
fi
mmd -i "$TMPD/p1.img" ::/boot
mmd -i "$TMPD/p1.img" ::/boot/grub
mcopy -i "$TMPD/p1.img" "$TMPD/candidate.pad" ::/boot/kernel
mcopy -i "$TMPD/p1.img" "$KERNEL" ::/boot/fallback
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

# Every staged chain, verified before the partition is committed: each of
# the five files must be whole (its cluster count matching its size) and
# the extents disjoint. mshowfat prints cluster ranges, the BPB names the
# cluster size, and the awk below fails closed: a missing record, a wrong
# count, or a shared cluster stops the build.
CFGBYTES=$(wc -c < "$GRUB_CFG" | tr -d ' ')
: > "$TMPD/chains.txt"
for f in ::/boot/kernel ::/boot/fallback ::/boot/grub/grub.cfg \
         ::/boot/grub/grubenv ::/boot/boot.cfg; do
    mshowfat -i "$TMPD/p1.img" "$f" >> "$TMPD/chains.txt"
done
# shellcheck disable=SC2046 # od output is exactly three numeric fields
set -- $(od -An -j 11 -N 3 -t u1 "$TMPD/p1.img")
CLBYTES=$(( ($1 + $2 * 256) * $3 ))
[ "$CLBYTES" -gt 0 ] || {
    echo "mkdisk_grub: the formatted volume reports a zero cluster size" >&2
    exit 1
}
awk -v cbytes="$CLBYTES" -v kernel="$SLOT_BYTES" -v fallback="$KBYTES" \
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

dd if="$TMPD/p1.img" of="$OUT.new" bs=1048576 seek="$GAP_MB" conv=notrunc 2>/dev/null

python3 "$REPO/tools/image/grub_embed.py" "$OUT.new" \
    "$TMPD/bios-boot.img" "$TMPD/disk-core.img"

dd if="$KRFFS_BIN" of="$OUT.new" bs=1048576 seek=$((GAP_MB + BOOT_MB)) \
    conv=notrunc 2>/dev/null

# Partition 3, when asked for: a FAT32 volume holding the recovery kernel
# and its RAM image, or the empty directories they will live in when this
# build was not given them.
if [ "$RECOVERY_MB" -gt 0 ]; then
    dd if=/dev/zero of="$TMPD/p3.img" bs=1048576 count="$RECOVERY_MB" 2>/dev/null
    if [ -n "$CLUSTER" ]; then
        mformat -i "$TMPD/p3.img" -F -N $FAT_SERIAL -c "$CLUSTER" ::
    else
        mformat -i "$TMPD/p3.img" -F -N $FAT_SERIAL ::
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
    dd if="$TMPD/p3.img" of="$OUT.new" bs=1048576 \
        seek=$((GAP_MB + BOOT_MB + KRFFS_MB)) conv=notrunc 2>/dev/null
fi
rm -rf "$TMPD"
mv "$OUT.new" "$OUT"

echo "mkdisk_grub: wrote $OUT"
echo "mkdisk_grub: boot it with rach2/run_from_disk.sh"
