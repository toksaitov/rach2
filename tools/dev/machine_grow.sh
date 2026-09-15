#!/bin/sh
# Run from recovery after the owner approves the reported devices.
# The plan stays in RAM; the backup volume is unmounted during the measurement.
set -eu
umask 077

fail() { echo "machine-grow: failed: $*" >&2; exit 1; }
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
work=$(mktemp -d /tmp/machine-grow.XXXXXX) || fail 'temporary directory'
echo "machine-grow: logs: $work"
cp "$plan/grow.plan" "$work/grow.plan"
cp "$plan/ranges.txt" "$work/ranges.txt"
sha256sum -c "$plan/programs.sha256" || fail 'the recovery programs differ from the prepared binaries'
read -r sectors serial start seed count disk_id seed_hash extra < "$work/grow.plan"
[ -z "${extra:-}" ] || fail 'extra plan fields'
for value in "$sectors" "$start" "$seed" "$count"; do
    case "$value" in ''|*[!0-9]*|0*) fail 'invalid plan number' ;; esac
    [ "${#value}" -le 10 ] && [ "$value" -le 4294967295 ] || fail 'plan number too large'
done
[ "$start" = 266240 ] && [ "$count" = "$((sectors - start))" ] \
    && [ "$seed" -lt "$count" ] || fail 'unexpected root geometry'
case "$serial" in ''|*[!A-Za-z0-9_-]*) fail 'invalid serial' ;; esac
case "$disk_id:$seed_hash" in *[!0-9a-f:]*) fail 'invalid plan hash or disk identifier' ;; esac
[ "${#disk_id}" = 8 ] && [ "${#seed_hash}" = 64 ] || fail 'short plan hash or disk identifier'
bytes=$(awk -v end="$sectors" -v root="$start" -v seed="$seed" '
    NF != 2 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ { exit 1 }
    NR == 1 && ($1 != 0 || $2 != 8) { exit 1 }
    NR == 2 && ($1 != root || $2 < seed) { exit 1 }
    $2 == 0 || $1 < previous || $1 + $2 > end { exit 1 }
    NR > 1 && $1 < root { exit 1 }
    { previous = $1 + $2; total += $2 }
    END { if (NR < 2) exit 1; printf "%.0f\n", total * 512 }
' "$work/ranges.txt") || fail 'invalid saved ranges'

# Identity survives a torn table, so restoration does not require that table.
cat /proc/disk > "$work/disk.before"
cat /proc/mounts > "$work/mounts.before"
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
source_device=$(awk -v m="$store" '$2 == m && $3 == "exfat" {print $1}' "$work/mounts.before")
case "$source_device" in
    "$disk"|"$disk"p*) fail 'the backup volume is on the target disk' ;;
    /dev/disk[0-9]|/dev/disk[0-9]p[0-9]) ;;
    *) fail 'the store must be a mounted exFAT volume on another disk' ;;
esac
if [ "$action" != restore ]; then
    [ "$(diskctl id "$disk")" = "$disk_id" ] || fail 'disk identifier changed'
    awk -v d="$number" -v s="$start" -v n="$count" '
        $1 == "part" && $2 == d {
            parts++
            if ($3 == 1 && $4 == 2048 && $5 == 131072 && $6 == "mbr:0c") good++
            if ($3 == 2 && $4 == 133120 && $5 == 131072 && $6 == "mbr:0c") good++
            if ($3 == 3 && $4 == s && $5 == n && $6 == "mbr:83") good++
        }
        END { if (parts != 3 || good != 3) exit 1 }
    ' "$work/disk.before" || fail 'the original partition layout changed'
fi
saved="$store/saved"

hash_file()
{
    hash_line=$(sha256sum "$1") || return 1
    printf '%s\n' "${hash_line%% *}"
}

# Each dd is bounded by bytes, even when a range starts inside a mebibyte.
pack()
{
    row=0
    while read -r first length; do
        row=$((row + 1))
        if [ "$((row % 128))" = 1 ]; then
            echo "machine-grow: reading: range $row, sector $first" >&2
        fi
        dd if="$disk" bs=1048576 skip="$((first * 512))" count="$((length * 512))" \
            iflag=skip_bytes,count_bytes 2>>"$work/reads.log" || return 1
    done < "$work/ranges.txt"
}

# Keep the producer's status; a digest alone cannot prove a complete read.
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

check_backup()
{
    [ "$(wc -c < "$saved/backup.bin")" = "$bytes" ] || fail 'short backup'
    [ "$(wc -c < "$saved/boot.img")" = 67108864 ] || fail 'short boot backup'
    cmp -s "$work/grow.plan" "$saved/grow.plan" || fail 'backup belongs to another plan'
    cmp -s "$work/ranges.txt" "$saved/ranges.txt" || fail 'backup uses other ranges'
    (cd "$saved" && sha256sum -c SHA256SUMS) || fail 'backup checksum'
    backup_hash=$(hash_file "$saved/backup.bin") || fail 'hashing the backup'
}

keep_recovery()
{
    bootctl --target "${disk}p1" env saved_entry recovery || fail 'saving recovery'
    bootctl --target "${disk}p1" cancel || fail 'cancelling the trial'
    bootctl --target "${disk}p1" status > "$work/boot.status" || fail 'reading boot state'
    cat "$work/boot.status"
    grep -q '^bootctl: saved_entry: recovery$' "$work/boot.status" || fail 'recovery is not saved'
    grep -q '^bootctl: next_entry: absent$' "$work/boot.status" || fail 'a trial remains armed'
}

run_logged()
{
    log_name=$1
    shift
    rm -f "$work/command.status"
    if (if "$@"; then echo 0 > "$work/command.status"; else echo "$?" > "$work/command.status"; fi) 2>&1 \
        | tee "$work/$log_name"; then
        [ "$(cat "$work/command.status")" = 0 ] || fail "$log_name"
    else
        fail "saving $log_name"
    fi
}

case "$action" in
    save)
        run_logged original-check.log /sbin/e2fsck -fn "${disk}p3"
        mkdir "$saved" || fail 'the backup directory already exists or cannot be created'
        cp "$work/grow.plan" "$work/ranges.txt" "$saved/"
        pack > "$saved/backup.bin" || fail 'saving disk ranges'
        [ "$(wc -c < "$saved/backup.bin")" = "$bytes" ] || fail 'short range copy'
        dd if="$disk" of="$saved/boot.img" bs=1048576 skip=1048576 count=67108864 \
            iflag=skip_bytes,count_bytes 2>"$work/boot-read.log" || fail 'saving the boot partition'
        [ "$(wc -c < "$saved/boot.img")" = 67108864 ] || fail 'short boot copy'
        bootctl --target "${disk}p1" status > "$saved/boot.status" || fail 'original boot state'
        sync
        (cd "$saved" && sha256sum backup.bin boot.img grow.plan ranges.txt > SHA256SUMS)
        check_backup
        check_disk_copy
        echo "machine-grow: result: $bytes range bytes and 67108864 boot bytes saved and checked"
        echo "machine-grow: next: copy saved to the host, check SHA256SUMS there, then return it as host.sha256"
        ;;
    grow)
        check_backup
        cmp -s "$saved/SHA256SUMS" "$saved/host.sha256" || fail 'host verification is absent'
        check_disk_copy
        [ "$(wc -c < "$store/seed.ext2")" = "$((seed * 512))" ] || fail 'seed size'
        [ "$(hash_file "$store/seed.ext2")" = "$seed_hash" ] || fail 'seed checksum'
        dd if="$saved/backup.bin" of="$work/mbr" bs=512 count=1 \
            2>"$work/mbr-read.log" || fail 'saved MBR'
        [ "$(wc -c < "$work/mbr")" = 512 ] || fail 'short saved MBR'
        for shift in 0 8 16 24; do
            octal=$(printf '%03o' "$(((seed >> shift) & 255))")
            printf '%b' "\\$octal"
        done > "$work/count"
        [ "$(wc -c < "$work/count")" = 4 ] || fail 'partition count encoding'
        dd if="$work/count" of="$work/mbr" bs=1 seek=490 count=4 conv=notrunc \
            2>"$work/mbr-edit.log" || fail 'preparing the temporary table'
        keep_recovery
        reflash -p "$disk" "$((start * 512))" "$((seed * 512))" < "$store/seed.ext2" \
            || fail 'writing the temporary seed; restore from the backup'
        seed_result=$(reflash --verify "$disk" "$((start * 512))" "$((seed * 512))") \
            || fail 'reading back the temporary seed'
        echo "$seed_result"
        [ "$(printf '%s\n' "$seed_result" | awk '$2 == "sha256" {print $3}')" = "$seed_hash" ] \
            || fail 'the temporary seed differs; restore from the backup'
        reflash "$disk" 0 512 < "$work/mbr" || fail 'writing the temporary table'
        diskctl reread "$disk" || fail 'rereading the temporary table'
        umount "$store" || fail 'unmounting the backup volume before timing'
        run_logged grow.log growroot "MBRID=$disk_id,3" --measure
        run_logged grown-check.log /sbin/e2fsck -fn "${disk}p3"
        mount -t exfat "$source_device" "$store" || fail 'remounting the backup volume'
        echo 'machine-grow: result: the measured grow and read-only check passed; recovery remains saved'
        echo 'machine-grow: next: restore the saved ranges before booting the installed system'
        ;;
    restore)
        check_backup
        cmp -s "$saved/SHA256SUMS" "$saved/host.sha256" || fail 'host verification is absent'
        offset=0
        while read -r first length; do
            amount=$((length * 512))
            if (if dd if="$saved/backup.bin" bs=1048576 skip="$offset" count="$amount" \
                    iflag=skip_bytes,count_bytes 2>>"$work/restore-reads.log"; then
                    echo 0 > "$work/restore-read.status"
                else
                    echo 1 > "$work/restore-read.status"
                fi) \
                | reflash "$disk" "$((first * 512))" "$amount"; then
                [ "$(cat "$work/restore-read.status")" = 0 ] || fail 'reading a backup range'
            else
                fail "restoring sectors $first+$length; recovery remains saved"
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
