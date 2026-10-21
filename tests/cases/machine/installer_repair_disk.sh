#!/bin/sh
#
# installer_repair_disk.sh: the edits on the host that put a disk of the
# installer-repair cases (tests/cases/machine.installer-repair.json) in the
# state a stopped update or conversion leaves: a root image given keys with
# debugfs, and a disk's FAT partitions changed with mtools and its root written
# with dd. A scenario of those cases runs it with every path and offset it
# needs as an argument.
#
# Usage:
#   installer_repair_disk.sh root-keys ROOT WORK
#   installer_repair_disk.sh legacy DISK RECOVERY ROOTAT ROOT NAME WORK PUBKEY
#   installer_repair_disk.sh slot DISK RECOVERY ROOTAT ROOT WORK
#   installer_repair_disk.sh keep DISK BOOT RECOVERY WORK
#
# RECOVERY, ROOTAT and BOOT are the first sectors of the disk's recovery
# partition, its root partition and its boot partition; ROOT is a root image,
# NAME the name the stick's volume gives it, WORK a directory of the
# scenario's own and PUBKEY root's public key. It exits 0 when every edit was
# made, and 1 with the edit that failed on standard error.
#

die() {
    echo "installer_repair_disk: $1" >&2
    exit 1
}

# root-keys: a home outside /home with a key, and one whose machine
# configuration has no keys, both in the root image, which an update must
# replace with the machine's.
root_keys() { # root_keys ROOT WORK
    echo image-key > "$2/image-key" || die "the image's key could not be written"
    for home in alice empty; do
        debugfs -w -R 'mkdir /srv' "$1" > /dev/null 2>&1
        debugfs -w -R "mkdir /srv/$home" "$1" > /dev/null 2>&1
        debugfs -w -R "mkdir /srv/$home/.ssh" "$1" > /dev/null 2>&1
        debugfs -w -R "write $2/image-key /srv/$home/.ssh/authorized_keys" "$1" > /dev/null 2>&1
        debugfs -R "cat /srv/$home/.ssh/authorized_keys" "$1" 2>/dev/null | grep -qx image-key ||
            die "the root image has no key in /srv/$home"
    done
}

# legacy: the archive the last update wrote in slot A, moved to the old
# layout's rach2-config with an account and an unowned key added, the image
# written over the root, and the mark of an update stopped after the root's
# write; then a slot A that has the account files and the mark but no
# generation, which the retry must clear while keeping the old archive.
legacy() { # legacy DISK RECOVERY ROOTAT ROOT NAME WORK PUBKEY
    vol="$1@@$(($2 * 512))"
    w=$6
    rm -rf "$w/saved-archive"
    mkdir -p "$w/saved-archive" || die "no directory for the saved archive"
    mcopy -s -i "$vol" '::rach2-config-a/*' "$w/saved-archive/" ||
        die "slot A of the archive could not be read"
    rm "$w/saved-archive/generation" || die "slot A has no generation"
    mdeltree -i "$vol" ::rach2-config-a || die "slot A could not be removed"
    mdeltree -i "$vol" ::rach2-config-b 2>/dev/null
    mmd -i "$vol" ::rach2-config || die "the old layout's directory could not be made"
    mcopy -s -i "$vol" "$w/saved-archive/"* ::rach2-config/ ||
        die "the archive could not be written in the old layout"
    mcopy -n -i "$vol" ::rach2-config/etc/passwd "$w/archived-passwd" ||
        die "the archived passwd could not be read off the recovery partition"
    echo "extra:x:1001:1001::/home/extra:/bin/sh" >> "$w/archived-passwd"
    mmd -D s -i "$vol" ::rach2-config/keys 2>/dev/null
    mmd -i "$vol" ::rach2-config/keys/2999 ||
        die "the unowned archived key directory could not be made"
    printf '%s\n' "$7" "# orphan-uid-2999" > "$w/orphan-key"
    mcopy -i "$vol" "$w/orphan-key" ::rach2-config/keys/2999/authorized_keys ||
        die "the unowned archived key could not be written"
    echo "$5 stopped" > "$w/update-in-progress"
    dd if="$4" of="$1" bs=512 seek="$3" conv=notrunc 2>/dev/null ||
        die "the image could not be written onto the root, as the stopped update left it"
    mcopy -o -i "$vol" "$w/archived-passwd" ::rach2-config/etc/passwd ||
        die "the archived passwd could not be written back to the recovery partition"
    mcopy -o -i "$vol" "$w/update-in-progress" ::rach2-config/update-in-progress ||
        die "the mark could not be planted on the recovery partition"
    mmd -i "$vol" ::rach2-config-a ::rach2-config-a/etc || die "the unfinished slot A could not be made"
    for account_file in passwd group shadow; do
        mcopy -i "$vol" "$w/saved-archive/etc/$account_file" "::rach2-config-a/etc/$account_file" ||
            die "the unfinished slot A has no $account_file"
    done
    mcopy -i "$vol" "$w/update-in-progress" ::rach2-config-a/update-in-progress ||
        die "the unfinished slot A has no mark"
}

# slot: the same stop with the newest archive in slot A: its unowned key
# replanted, the image written over the root again and slot A marked.
slot() { # slot DISK RECOVERY ROOTAT ROOT WORK
    vol="$1@@$(($2 * 512))"
    mmd -D s -i "$vol" ::rach2-config-a/keys 2>/dev/null
    mmd -i "$vol" ::rach2-config-a/keys/2999 || die "the orphan directory could not be replanted"
    mcopy -i "$vol" "$5/orphan-key" ::rach2-config-a/keys/2999/authorized_keys ||
        die "the orphan key could not be replanted"
    dd if="$4" of="$1" bs=512 seek="$3" conv=notrunc 2>/dev/null ||
        die "the image could not be written onto the root"
    mcopy -o -i "$vol" "$5/update-in-progress" ::rach2-config-a/update-in-progress ||
        die "the mark could not be planted in slot A"
}

# keep: a conversion stopped after the boot partition's files were kept on
# the recovery partition, marked whole, and before its new file system had
# them: the boot partition's first sectors zeroed.
keep() { # keep DISK BOOT RECOVERY WORK
    k="$4/boot-keep"
    rm -rf "$k"
    mkdir -p "$k/files" || die "no directory for the kept files"
    dd if="$1" of="$k/sector" bs=512 skip="$2" count=1 2>/dev/null ||
        die "the boot partition's first sector could not be read"
    echo whole > "$k/complete"
    mcopy -s -n -i "$1@@$(($2 * 512))" "::*" "$k/files/" ||
        die "the boot partition's files could not be read"
    mcopy -s -i "$1@@$(($3 * 512))" "$k" ::/rach2-boot-keep ||
        die "the kept files could not be put on the recovery partition"
    dd if=/dev/zero of="$1" bs=512 seek="$2" count=64 conv=notrunc 2>/dev/null ||
        die "the boot partition's file system could not be zeroed"
}

case "${1:-}" in
    root-keys) [ $# -eq 3 ] && root_keys "$2" "$3" ;;
    legacy) [ $# -eq 8 ] && legacy "$2" "$3" "$4" "$5" "$6" "$7" "$8" ;;
    slot) [ $# -eq 6 ] && slot "$2" "$3" "$4" "$5" "$6" ;;
    keep) [ $# -eq 5 ] && keep "$2" "$3" "$4" "$5" ;;
    *) false ;;
esac || die "usage: installer_repair_disk.sh root-keys|legacy|slot|keep ARGUMENTS"
