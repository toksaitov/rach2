#!/bin/sh
#
# machine_grow.sh: save the sectors an ext2 grow writes on an installed disk,
# grow its root in place under a timer, and put the saved sectors back. It
# runs in the recovery system, as root.
#
# Usage: machine_grow.sh save|grow|restore DISK STORE PLAN-DIRECTORY
#
# DISK is a whole-disk node, /dev/diskN, with an MBR or a GUID table, as the
# plan's disk identifier says. STORE is the mount point of an exFAT
# volume on another disk, with the seed root as seed.ext2. PLAN-DIRECTORY,
# copied under /tmp, is what machine_grow_plan.sh made; tools/Readme.md gives
# the procedure. Run it only after the owner has approved the devices it
# reports. Each run writes its logs to a new machine-grow.* directory on
# STORE, beside the backup, so only the plan is in RAM.
#
# `save` copies the ranges and the 64 MiB boot partition to STORE/saved and
# checks the copy against the disk; `grow` and `restore` require the host's
# check of that copy, STORE/saved/host.sha256. `grow` makes the boot
# partition start recovery, writes the seed and a partition table whose root
# is the seed's size, an edited copy of the saved MBR or the GUID table that
# `diskctl shrink` writes, and times `growroot --measure`. `restore` writes the
# saved ranges back and checks the result. The exit status is 0 when the
# action finished, 1 when it failed, and 129, 130 or 143 after a hangup, an
# interrupt or a termination.
#
set -eu
umask 077
work=

# alert TEXT: print a failure on stderr and, when there is one, on
# /dev/console, so that a failure shows even when the output goes to a
# transcript; the console needs no space on either file system. A write that
# fails is ignored.
alert()
{
    printf 'machine-grow: failed: %s\n' "$*" >&2 || :
    if [ -c /dev/console ]; then
        printf 'machine-grow: failed: %s\n' "$*" > /dev/console || :
    fi
}
# fail TEXT: alert and exit 1.
fail() { alert "$*"; exit 1; }
# The EXIT trap: write the exit status to WORK/result.status once the work
# directory exists, report a failure, and exit with the status, or with 1
# when the script stopped before its end with status 0.
finish()
{
    result=$?
    trap - EXIT
    [ -n "$finished" ] || [ "$result" -ne 0 ] || result=1
    if [ -n "${work:-}" ]; then
        if ! printf '%s\n' "$result" > "$work/result.status"; then
            alert "writing $work/result.status"
            result=1
        fi
    fi
    if [ "$result" != 0 ]; then
        alert "stopped with status $result; logs: ${work:-not created}"
    fi
    exit "$result"
}
# BusyBox's ash, which runs this in recovery, reports a failing status when
# `set -u` stops the script; bash 3.2, the Mac's sh, gives the trap 0, and the
# style check asks for the flag in a script under tools/ that sets both flags
# and traps EXIT, so `finish` fails a run that reached no success exit.
finished=
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
[ "$#" = 4 ] || fail 'usage: machine_grow.sh save|grow|restore DISK STORE PLAN-DIRECTORY'
action=$1
disk=$2
store=$3
plan=$4
case "$action" in save|grow|restore) ;; *) fail 'unknown action' ;; esac
case "$disk" in /dev/disk[0-9]) ;; *) fail 'name a whole-disk node' ;; esac
case "$plan" in /tmp/*) ;; *) fail 'the plan must be copied into /tmp first' ;; esac
case "$store" in /*) ;; *) fail 'the store must be an absolute mount point' ;; esac
[ "$(id -u)" = 0 ] || fail 'root is required'
[ -b "$disk" ] || fail 'the whole-disk window is not published'
number=${disk#/dev/disk}
# Resolve STORE, require it to be a mounted exFAT volume on a disk other than
# DISK, and make the work directory there with copies of the plan, setting
# `store`, `source_device` and `work`. The mount is checked before anything is
# created below it, so an unmounted directory or a symlink into the target
# never becomes the work directory. A MiB is written and removed before any
# write to the target, since a free-space estimate does not prove allocation;
# later writes still check their errors.
prepare_work()
{
    store=$(CDPATH='' cd -- "$store" && pwd -P) || fail 'the store directory'
    store=${store%/}
    source_device=$(awk -v m="$store" '$2 == m && $3 == "exfat" {print $1}' /proc/mounts) \
        || fail 'reading the store mount'
    case "$source_device" in
        "$disk"|"$disk"p*) fail 'the backup volume is on the target disk' ;;
        /dev/disk[0-9]|/dev/disk[0-9]p[0-9]) ;;
        *) fail 'the store must be a mounted exFAT volume on another disk' ;;
    esac
    cd / || fail 'leaving the store before measurement'
    work=$(mktemp -d "$store/machine-grow.XXXXXX") || fail "creating work directory on $store"
    echo "machine-grow: logs: $work"
    cp "$plan/grow.plan" "$work/grow.plan" || fail "writing $work/grow.plan"
    cp "$plan/ranges.txt" "$work/ranges.txt" || fail "writing $work/ranges.txt"
    dd if=/dev/zero of="$work/space.reserve" bs=65536 count=16 2>"$work/space.log" \
        || fail "reserving log space on $store"
    [ "$(wc -c < "$work/space.reserve" | tr -d ' ')" = 1048576 ] || fail 'short log-space reservation'
    rm "$work/space.reserve" || fail 'releasing the log-space reservation'
}
prepare_work
sha256sum -c "$plan/programs.sha256" || fail 'the recovery programs differ from the prepared binaries'
read -r sectors serial start seed count disk_id seed_hash extra < "$work/grow.plan"
[ -z "${extra:-}" ] || fail 'extra plan fields'
for value in "$sectors" "$start" "$seed" "$count"; do
    case "$value" in ''|*[!0-9]*|0*) fail 'invalid plan number' ;; esac
    [ "${#value}" -le 10 ] && [ "$value" -le 4294967295 ] || fail 'plan number too large'
done
case "$serial" in ''|*[!A-Za-z0-9_-]*) fail 'invalid serial' ;; esac
case "$seed_hash" in *[!0-9a-f]*) fail 'invalid plan hash' ;; esac
[ "${#seed_hash}" = 64 ] || fail 'short plan hash'
# The table the disk identifier names: an MBR, whose signature has eight
# digits, or a GUID table. A GUID table's root ends before the backup array
# and header in the disk's last 33 sectors, the backup is the last saved
# range, and the first saved range is sectors 0 to 39, the protective MBR
# and the primary table in whole 4096-byte blocks, where an MBR's is sector
# 0 in one block.
h='[0-9a-f]'
h4=$h$h$h$h
# The patterns are meant as globs.
# shellcheck disable=SC2254
case "$disk_id" in
    $h4$h4) table=mbr; table_sectors=8; root_end=$sectors; identity="MBRID=$disk_id" ;;
    $h4$h4-$h4-$h4-$h4-$h4$h4$h4)
        table=gpt; table_sectors=40; root_end=$((sectors - 33)); identity="GPTID=$disk_id" ;;
    *) fail 'invalid disk identifier' ;;
esac
[ "$start" = 266240 ] && [ "$count" = "$((root_end - start))" ] \
    && [ "$seed" -lt "$count" ] || fail 'unexpected root geometry'
bytes=$(awk -v end="$sectors" -v root="$start" -v seed="$seed" -v table="$table_sectors" '
    NF != 2 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ { exit 1 }
    NR == 1 && ($1 != 0 || $2 != table) { exit 1 }
    NR == 2 && ($1 != root || $2 < seed) { exit 1 }
    $2 == 0 || $1 < previous || $1 + $2 > end { exit 1 }
    NR > 1 && $1 < root { exit 1 }
    { previous = $1 + $2; total += $2 }
    END { if (NR < 2) exit 1; printf "%.0f\n", total * 512 }
' "$work/ranges.txt") || fail 'invalid saved ranges'

# Identity survives a torn table, so restoration does not require that table.
cat /proc/disk > "$work/disk.before" || fail "writing $work/disk.before"
cat /proc/mounts > "$work/mounts.before" || fail "writing $work/mounts.before"
awk -v d="$number" -v n="$sectors" '
    $1 == "disk" && $2 == d { found++; if ($5 != n) exit 1 }
    END { if (found != 1) exit 1 }
' "$work/disk.before" || fail 'disk size or registry record changed'
awk -v d="$number" -v serial="$serial" '
    $1 == "identity" && $2 == d {
        found++; if ($3 != 512 || $4 != "fixed" || $5 != "free" || $6 != serial) exit 1
    }
    END { if (found != 1) exit 1 }
' "$work/disk.before" || fail 'disk serial, sector size or mount state changed'
if [ "$action" != restore ]; then
    [ "$(diskctl id "$disk")" = "$disk_id" ] || fail 'disk identifier changed'
    # A GUID table's entries are listed by their partition GUIDs, not their
    # types; entry 4 is the BIOS boot partition before the first megabyte.
    awk -v d="$number" -v s="$start" -v n="$count" -v table="$table" '
        $1 == "disk" && $2 == d && $6 == table { kind++ }
        $1 == "part" && $2 == d && table == "mbr" {
            parts++
            if ($3 == 1 && $4 == 2048 && $5 == 131072 && $6 == "mbr:0c") good++
            if ($3 == 2 && $4 == 133120 && $5 == 131072 && $6 == "mbr:0c") good++
            if ($3 == 3 && $4 == s && $5 == n && $6 == "mbr:83") good++
        }
        $1 == "part" && $2 == d && table == "gpt" {
            parts++
            if ($3 == 1 && $4 == 2048 && $5 == 131072) good++
            if ($3 == 2 && $4 == 133120 && $5 == 131072) good++
            if ($3 == 3 && $4 == s && $5 == n) good++
            if ($3 == 4 && $4 == 34 && $5 == 2014) good++
        }
        END { if (kind != 1 || parts != good || good != (table == "mbr" ? 3 : 4)) exit 1 }
    ' "$work/disk.before" || fail 'the original partition layout changed'
fi
saved="$store/saved"

# hash_file FILE: print the SHA-256 of FILE; status 1 when sha256sum fails.
hash_file()
{
    hash_line=$(sha256sum "$1") || return 1
    printf '%s\n' "${hash_line%% *}"
}

# Write the ranges of ranges.txt, read from DISK, to stdout, with a progress
# line on stderr every 128 ranges; status 1 when a read fails. Each dd is
# bounded by bytes, even when a range starts inside a mebibyte.
pack()
{
    row=0
    while read -r first length; do
        row=$((row + 1))
        if [ "$((row % 128))" = 1 ]; then
            echo "machine-grow: reading: range $row, sector $first" >&2
        fi
        dd status=none if="$disk" bs=1048576 skip="$((first * 512))" count="$((length * 512))" \
            iflag=skip_bytes,count_bytes 2>>"$work/reads.log" || return 1
    done < "$work/ranges.txt"
}

# Hash the ranges as DISK has them now, and fail unless the digest is the
# backup's, `backup_hash`, which check_backup sets. The reader's status is
# kept apart from the pipeline's, since a digest alone cannot prove a
# complete read.
check_disk_copy()
{
    rm -f "$work/read.status"
    if (if pack; then echo 0 > "$work/read.status"; else echo 1 > "$work/read.status"; fi) \
        | sha256sum > "$work/disk.sha256"; then
        [ "$(cat "$work/read.status")" = 0 ] || fail 'reading the saved disk ranges'
    else
        fail 'hashing the saved disk ranges'
    fi
    read -r actual _ < "$work/disk.sha256"
    [ "$actual" = "$backup_hash" ] || fail 'the disk ranges differ from the saved copy'
}

# Fail unless STORE/saved is a whole backup made for this plan: the sizes of
# its copies, its copies of the plan and the ranges, and its SHA256SUMS. Set
# `backup_hash` to the digest of backup.bin.
check_backup()
{
    [ "$(wc -c < "$saved/backup.bin")" = "$bytes" ] || fail 'short backup'
    [ "$(wc -c < "$saved/boot.img")" = 67108864 ] || fail 'short boot backup'
    cmp -s "$work/grow.plan" "$saved/grow.plan" || fail 'backup belongs to another plan'
    cmp -s "$work/ranges.txt" "$saved/ranges.txt" || fail 'backup uses other ranges'
    (cd "$saved" && sha256sum -c SHA256SUMS) || fail 'backup checksum'
    backup_hash=$(hash_file "$saved/backup.bin") || fail 'hashing the backup'
}

# Fail unless the seed has the plan's length and digest, and growset, run on
# the seed and the root's size, gives the plan's ranges, with a GUID table's
# first range widened and its backup added. The ranges come from the seed
# and the target's geometry, not from a digest supplied with them.
check_plan()
{
    [ "$(wc -c < "$store/seed.ext2" | tr -d ' ')" = "$((seed * 512))" ] || fail 'seed length'
    [ "$(hash_file "$store/seed.ext2")" = "$seed_hash" ] || fail 'seed checksum'
    /bin/growset --save --ranges --lba "$start" "$store/seed.ext2" "$count" \
        > "$work/expected-ranges.txt" 2> "$work/growset.log" || {
            cat "$work/growset.log" >&2 || :
            fail "creating $work/expected-ranges.txt; growset failed (see $work/growset.log)"
        }
    if [ "$table" = gpt ]; then
        awk -v first="$table_sectors" -v end="$sectors" '
            NR == 1 && $1 == 0 { $2 = first }
            { print }
            END { print end - 33, 33 }
        ' "$work/expected-ranges.txt" > "$work/table-ranges.txt" \
            || fail "creating $work/table-ranges.txt"
        mv "$work/table-ranges.txt" "$work/expected-ranges.txt" \
            || fail "replacing $work/expected-ranges.txt"
    fi
    cmp "$work/ranges.txt" "$work/expected-ranges.txt" || fail 'ranges differ from the seed and target geometry'
    rm "$work/expected-ranges.txt"
}

# hash_slice FILE OFFSET COUNT: print the SHA-256 of COUNT bytes of FILE from
# byte OFFSET; status 1 when `reflash --verify` fails or prints a line of
# another form. reflash's verifier requires every byte of the extent to read,
# and its bounded buffer needs no copy of the extent in recovery's RAM file
# system.
hash_slice()
{
    slice_result=$(reflash --verify "$1" "$2" "$3") || return 1
    printf '%s\n' "$slice_result" | awk -v offset="$2" -v count="$3" '
        { n++ }
        NF != 8 || $1 != "reflash:" || $2 != "sha256" || length($3) != 64 ||
            $3 ~ /[^0-9a-f]/ || $4 != "of" || $5 != count || $6 != "bytes" ||
            $7 != "at" || $8 != offset { bad = 1 }
        { digest = $3 }
        END { if (bad || n != 1) exit 1; print digest }
    '
}

# Fail unless the boot partition on DISK is the saved boot.img.
check_boot_copy()
{
    expected_boot=$(hash_file "$saved/boot.img") || fail 'hashing the boot backup'
    actual_boot=$(hash_slice "$disk" 1048576 67108864) || fail 'reading the complete boot partition'
    [ "$actual_boot" = "$expected_boot" ] || fail 'the boot backup differs from the disk'
}

# Make WORK/recovery.env from the saved boot partition with growboot: the
# 1024-byte GRUB environment block with `recovery` as its saved entry and no
# `next_entry`. Set `env_offset` to the block's byte offset in the partition.
# growboot reads the saved FAT layout and runs bootctl's editor against a
# memory overlay, so only the block is copied.
prepare_boot()
{
    /bin/growboot "$saved/boot.img" "$work/recovery.env" > "$work/prepared-boot.status" || fail 'preparing recovery from boot.img'
    env_sector=$(awk '$1 == "growboot:" && $2 == "environment:" {n++; sector=$3}
        END {if (n != 1) exit 1; print sector}' "$work/prepared-boot.status") || fail 'saved environment extent'
    case "$env_sector" in ''|*[!0-9]*) fail 'invalid saved environment extent' ;; esac
    [ "$env_sector" -gt 0 ] && [ "$env_sector" -le 131070 ] || fail 'saved environment bounds'
    [ "$(wc -c < "$work/recovery.env" | tr -d ' ')" = 1024 ] || fail 'prepared environment length'
    env_offset=$((env_sector * 512))
}

# Make the boot partition on DISK the saved one with the prepared environment
# block, so that it starts recovery. The extent before the block, the block
# and the extent after it are each compared with their source, written only
# when they differ, and read back; a block on the disk that is plain text and
# already saves recovery is kept. Fail unless bootctl then reports recovery
# saved and no trial armed. The block is compared on its own, so a verified
# recovery entry is never rewritten while another boot extent is repaired. A
# damaged header need not parse: every byte written comes from the verified
# backup or the prepared block.
keep_recovery()
{
    for part in prefix environment suffix; do
        source_file="$saved/boot.img"
        case "$part" in
            prefix) at=0; amount=$env_offset ;;
            environment) at=$env_offset; amount=1024; source_file="$work/recovery.env" ;;
            suffix) at=$((env_offset + 1024)); amount=$((67108864 - at)) ;;
        esac
        source_at=$at
        [ "$part" != environment ] || source_at=0
        [ "$amount" -gt 0 ] || continue
        if [ "$part" = environment ]; then
            dd status=none if="$disk" of="$work/boot-current" bs=1048576 skip="$((1048576 + at))" count=1024 \
                iflag=skip_bytes,count_bytes 2>"$work/env-read.log" || fail 'reading the environment'
            [ "$(wc -c < "$work/boot-current" | tr -d ' ')" = 1024 ] || fail 'short environment read'
            LC_ALL=C tr -d '\012\040-\176' < "$work/boot-current" > "$work/boot-nontext" || fail 'checking environment bytes'
            if [ ! -s "$work/boot-nontext" ] && awk '
                NR == 1 { if ($0 != "# GRUB Environment Block") bad = 1; next }
                /^#[ -~]*$/ { next }
                /^saved_entry=recovery$/ { saved++; next }
                /^(saved_entry|next_entry)=/ { bad = 1; next }
                /^[A-Za-z_][A-Za-z0-9_]*=[ -~]*$/ { next }
                { bad = 1 }
                END { exit (bad || saved != 1) }
            ' "$work/boot-current"; then
                cp "$work/boot-current" "$work/recovery.env" || fail 'preserving recovery state'
            fi
        fi
        wanted=$(hash_slice "$source_file" "$source_at" "$amount") || fail 'reading the saved boot extent'
        current=$(hash_slice "$disk" "$((1048576 + at))" "$amount") || fail 'reading the current boot extent'
        if [ "$wanted" = "$current" ]; then
            echo "machine-grow: boot $part unchanged"
        else
            rm -f "$work/boot-write.status"
            if (if dd status=none if="$source_file" bs=1048576 skip="$source_at" count="$amount" \
                    iflag=skip_bytes,count_bytes 2>>"$work/boot-write.log"; then
                    echo 0 > "$work/boot-write.status"
                else
                    echo 1 > "$work/boot-write.status"
                fi) | reflash "$disk" "$((1048576 + at))" "$amount"; then
                [ "$(cat "$work/boot-write.status")" = 0 ] || fail 'reading the boot repair source'
            else
                fail "repairing boot $part"
            fi
            current=$(hash_slice "$disk" "$((1048576 + at))" "$amount") || fail 'reading back the boot repair'
            [ "$wanted" = "$current" ] || fail 'boot repair readback differs'
            echo "machine-grow: boot $part repaired from boot.img"
        fi
    done
    bootctl --target "${disk}p1" status > "$work/boot.status" || fail 'boot status'
    cat "$work/boot.status"
    grep -q '^bootctl: saved_entry: recovery$' "$work/boot.status" || fail 'recovery is not saved'
    grep -q '^bootctl: next_entry: absent$' "$work/boot.status" || fail 'a trial is still armed'
}

# run_logged LOG COMMAND...: run COMMAND with its output appended to
# WORK/LOG, and fail when it fails or the log cannot be written. tee checks
# every write of the output, and the status file keeps the command's own
# status. Appending lets thousands of restore calls share one file.
run_logged()
{
    log_name=$1
    shift
    rm -f "$work/command.status"
    if (if "$@"; then echo 0 > "$work/command.status"; else echo "$?" > "$work/command.status"; fi) 2>&1 \
        | tee -a "$work/$log_name" > /dev/null; then
        [ "$(cat "$work/command.status")" = 0 ] || fail "command failed; see $work/$log_name"
    else
        fail "writing $work/$log_name"
    fi
}

# Unmount STORE, run `growroot --measure` on the root of DISK with its output
# held in memory, remount STORE, and write the output to WORK/grow.log and
# stdout. Fail, after remounting, when the grow fails, its final status line
# is missing, or its output passes 64 KiB. Unmounting keeps the backup
# volume's log writes out of the disk counters, and no file descriptor on
# STORE is open during the capture.
run_measured()
{
    umount -n "$store" || fail 'unmounting the backup volume before timing'
    measured_output=$(
        { if growroot "$identity,3" --measure; then code=0; else code=$?; fi
          printf '\nmachine-grow-status: %s\n' "$code"
        } 2>&1 | head -c 65537
    ) || { mount -n -t exfat "$source_device" "$store"; fail 'capturing the grow output'; }
    mount -n -t exfat "$source_device" "$store" || fail 'remounting the backup volume; measurement is not saved'
    printf '%s\n' "$measured_output" | tee "$work/grow.log" > /dev/null \
        || fail "writing $work/grow.log"
    [ "${#measured_output}" -le 65536 ] || fail "grow output exceeds 64 KiB; see $work/grow.log"
    measured_status=${measured_output##*'
'}
    [ "$measured_status" = 'machine-grow-status: 0' ] || fail "measured grow failed; see $work/grow.log"
    printf '%s\n' "$measured_output" || fail 'writing the grow report'
    unset measured_output
}

# Write `amount` bytes of the backup, from byte `offset`, to DISK at sector
# `first`; status 1 when the read, the status file or the write fails. The
# caller logs reflash's whole output for each range in WORK/restore.log.
restore_range()
{
    rm -f "$work/restore-read.status" || return 1
    if (if dd status=none if="$saved/backup.bin" bs=1048576 skip="$offset" count="$amount" \
            iflag=skip_bytes,count_bytes 2>>"$work/restore-reads.log"; then
            echo 0 > "$work/restore-read.status"
        else
            echo 1 > "$work/restore-read.status"
        fi) | reflash "$disk" "$((first * 512))" "$amount"; then
        [ "$(cat "$work/restore-read.status")" = 0 ] || return 1
    else
        return 1
    fi
}

check_plan

case "$action" in
    save)
        run_logged original-check.log /sbin/e2fsck -fn "${disk}p3"
        [ ! -e "$saved" ] || fail 'the backup directory already exists'
        saved="$store/saved.pending"
        mkdir "$saved" || fail 'preserve the interrupted saved.pending directory before saving again'
        cp "$work/grow.plan" "$work/ranges.txt" "$saved/"
        pack > "$saved/backup.bin" || fail 'saving disk ranges'
        [ "$(wc -c < "$saved/backup.bin")" = "$bytes" ] || fail 'short range copy'
        dd status=none if="$disk" of="$saved/boot.img" bs=1048576 skip=1048576 count=67108864 \
            iflag=skip_bytes,count_bytes 2>"$work/boot-read.log" || fail 'saving the boot partition'
        [ "$(wc -c < "$saved/boot.img")" = 67108864 ] || fail 'short boot copy'
        bootctl --target "${disk}p1" status > "$saved/boot.status" || fail 'original boot state'
        sync
        (cd "$saved" && sha256sum backup.bin boot.img grow.plan ranges.txt > SHA256SUMS)
        check_backup
        check_disk_copy
        check_boot_copy
        prepare_boot
        sync
        mv "$saved" "$store/saved" || fail 'publishing the checked backup'
        sync
        saved="$store/saved"
        echo "machine-grow: result: $bytes range bytes and 67108864 boot bytes saved and checked"
        echo "machine-grow: next: copy saved to the host, check SHA256SUMS there, then return it as host.sha256"
        ;;
    grow)
        check_backup
        cmp -s "$saved/SHA256SUMS" "$saved/host.sha256" || fail 'host verification is absent'
        check_disk_copy
        check_boot_copy
        umount -n "$store" || fail 'the store is busy; close its files before grow'
        mount -n -t exfat "$source_device" "$store" || fail 'remounting the store before grow'
        prepare_boot
        if [ "$table" = mbr ]; then
            dd status=none if="$saved/backup.bin" of="$work/mbr" bs=512 count=1 \
                2>"$work/mbr-read.log" || fail 'saved MBR'
            [ "$(wc -c < "$work/mbr")" = 512 ] || fail 'short saved MBR'
            for shift in 0 8 16 24; do
                octal=$(printf '%03o' "$(((seed >> shift) & 255))")
                printf '%b' "\\$octal"
            done > "$work/count"
            [ "$(wc -c < "$work/count")" = 4 ] || fail 'partition count encoding'
            dd status=none if="$work/count" of="$work/mbr" bs=1 seek=490 count=4 conv=notrunc \
                2>"$work/mbr-edit.log" || fail 'preparing the temporary table'
        fi
        keep_recovery
        run_logged seed-write.log reflash -p "$disk" "$((start * 512))" "$((seed * 512))" < "$store/seed.ext2" \
            || fail 'writing the temporary seed; restore from the backup'
        seed_result=$(reflash --verify "$disk" "$((start * 512))" "$((seed * 512))") \
            || fail 'reading back the temporary seed'
        echo "$seed_result"
        [ "$(printf '%s\n' "$seed_result" | awk '$2 == "sha256" {print $3}')" = "$seed_hash" ] \
            || fail 'the temporary seed differs; restore from the backup'
        if [ "$table" = mbr ]; then
            reflash "$disk" 0 512 < "$work/mbr" || fail 'writing the temporary table'
            diskctl reread "$disk" || fail 'rereading the temporary table'
        else
            run_logged table.log diskctl shrink "$disk" 3 "$seed"
        fi
        run_measured
        run_logged grown-check.log /sbin/e2fsck -fn "${disk}p3"
        echo 'machine-grow: result: the measured grow and read-only check passed; recovery remains saved'
        echo 'machine-grow: next: restore the saved ranges before booting the installed system'
        ;;
    restore)
        check_backup
        cmp -s "$saved/SHA256SUMS" "$saved/host.sha256" || fail 'host verification is absent'
        prepare_boot
        offset=0
        row=0
        while read -r first length; do
            row=$((row + 1))
            amount=$((length * 512))
            run_logged restore.log restore_range
            if [ "$((row % 128))" = 1 ]; then
                echo "machine-grow: restored: range $row, sector $first"
            fi
            offset=$((offset + amount))
            if [ "$first" = 0 ]; then
                diskctl reread "$disk" || fail 'rereading the restored table'
                keep_recovery
            fi
        done < "$work/ranges.txt"
        [ "$offset" = "$bytes" ] || fail 'restoration length'
        check_disk_copy
        run_logged restored-check.log /sbin/e2fsck -fn "${disk}p3"
        echo 'machine-grow: result: every saved range matches and the restored root checks clean'
        echo 'machine-grow: next: recovery remains saved; inspect the record before a candidate trial'
        ;;
esac
finished=1
