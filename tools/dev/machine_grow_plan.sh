#!/bin/sh
# Prepare bounded ranges from the seed; this command reads only host files.
set -eu
umask 077

fail() { echo "machine-grow-plan: failed: $*" >&2; exit 1; }
[ "$#" = 5 ] || fail 'usage: machine_grow_plan.sh SEED DISK-SECTORS SERIAL MBR-ID DIRECTORY'
seed=$1
sectors=$2
serial=$3
disk_id=$4
directory=$5
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
case "$sectors" in ''|*[!0-9]*|0*) fail 'invalid disk size' ;; esac
[ "${#sectors}" -le 10 ] && [ "$sectors" -le 4294967295 ] || fail 'disk size too large'
case "$serial" in ''|*[!A-Za-z0-9_-]*) fail 'invalid serial' ;; esac
case "$disk_id" in *[!0-9a-f]*) fail 'invalid MBR identifier' ;; esac
[ "${#disk_id}" = 8 ] || fail 'the MBR identifier must have eight digits'
[ -f "$seed" ] || fail 'the seed is not a regular file'
seed_bytes=$(wc -c < "$seed" | tr -d ' ')
[ "$((seed_bytes % 512))" = 0 ] || fail 'the seed is not sector aligned'
start=266240
count=$((sectors - start))
seed_sectors=$((seed_bytes / 512))
[ "$seed_sectors" -gt 0 ] && [ "$seed_sectors" -lt "$count" ] || fail 'the seed must grow'
mkdir "$directory" || fail 'the output directory must be new'
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
e2fsck -fn "$seed" > "$directory/seed-check.log" 2>&1 || fail 'the seed is not clean'
sum_line=$(shasum -a 256 "$seed") || fail 'seed checksum'
seed_hash=${sum_line%% *}
printf '%s %s %s %s %s %s %s\n' \
    "$sectors" "$serial" "$start" "$seed_sectors" "$count" "$disk_id" "$seed_hash" \
    > "$directory/grow.plan"
"$repo/build/tools/inspect/growset" --lba "$start" "$seed" "$count" \
    > "$directory/grow-set.txt" || fail 'growset'
printf '%s %s\n' "$start" "$seed_sectors" > "$directory/ranges.unsorted"
awk '$1 == "range" {print $2, $3}' "$directory/grow-set.txt" >> "$directory/ranges.unsorted"
sort -n -k1,1 "$directory/ranges.unsorted" > "$directory/ranges.sorted"
awk '
    NR == 1 { first = $1; end = $1 + $2; next }
    $1 <= end { if ($1 + $2 > end) end = $1 + $2; next }
    { printf "%.0f %.0f\n", first, end - first; first = $1; end = $1 + $2 }
    END { printf "%.0f %.0f\n", first, end - first }
' "$directory/ranges.sorted" > "$directory/ranges.txt"
rm "$directory/ranges.sorted" "$directory/ranges.unsorted"
cp "$repo/tools/dev/machine_grow.sh" "$directory/machine_grow.sh"
printf '%s  seed.ext2\n' "$seed_hash" > "$directory/seed.sha256"
for pair in utils/growroot:/bin/growroot utils/reflash:/bin/reflash \
    utils/diskctl:/bin/diskctl utils/sbin/bootctl:/bin/bootctl \
    vendor/e2fsprogs/e2fsck:/sbin/e2fsck vendor/e2fsprogs/resize2fs:/sbin/resize2fs; do
    sum_line=$(shasum -a 256 "$repo/build/${pair%%:*}") || fail 'recovery program checksum'
    printf '%s  %s\n' "${sum_line%% *}" "${pair#*:}"
done > "$directory/programs.sha256"
awk '{total += $2} END {printf "machine-grow-plan: result: %.0f sectors, %.0f bytes plus the boot partition\n", total, total * 512}' \
    "$directory/ranges.txt"
