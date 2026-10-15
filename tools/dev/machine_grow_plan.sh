#!/bin/sh
#
# machine_grow_plan.sh: make, on the host, the plan that machine_grow.sh
# follows on an installed machine.
#
# Usage: machine_grow_plan.sh SEED DISK-SECTORS SERIAL DISK-ID DIRECTORY
#
# DISK-ID is what `diskctl id` prints on the machine: an MBR's signature, as
# eight hexadecimal digits, or a GUID table's disk GUID, and it selects the
# layout the disk must have. The new DIRECTORY receives grow.plan, the
# bounded sector ranges the seed's write and the grow change as ranges.txt,
# the seed's digest, a copy of machine_grow.sh and the digests of the
# recovery programs. The script reads only files on the host.
set -eu
umask 077

# fail TEXT: print the failure and exit 1.
fail() { echo "machine-grow-plan: failed: $*" >&2; exit 1; }
[ "$#" = 5 ] || fail 'usage: machine_grow_plan.sh SEED DISK-SECTORS SERIAL DISK-ID DIRECTORY'
seed=$1
sectors=$2
serial=$3
disk_id=$4
directory=$5
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
# The recovery programs are the architecture's, from ARCH as the build takes
# it (mk/arch.sh): build/ for i686, build/x86_64/ for x86-64.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
# shellcheck source=mk/arch.sh
. "$repo/mk/arch.sh"
case "$sectors" in ''|*[!0-9]*|0*) fail 'invalid disk size' ;; esac
[ "${#sectors}" -le 10 ] && [ "$sectors" -le 4294967295 ] || fail 'disk size too large'
case "$serial" in ''|*[!A-Za-z0-9_-]*) fail 'invalid serial' ;; esac
h='[0-9a-f]'
h4=$h$h$h$h
# The patterns are meant as globs.
# shellcheck disable=SC2254
case "$disk_id" in
    $h4$h4) table=mbr ;;
    $h4$h4-$h4-$h4-$h4-$h4$h4$h4) table=gpt ;;
    *) fail 'the disk identifier is neither an MBR signature nor a disk GUID' ;;
esac
[ -f "$seed" ] || fail 'the seed is not a regular file'
seed_bytes=$(wc -c < "$seed" | tr -d ' ')
[ "$((seed_bytes % 512))" = 0 ] || fail 'the seed is not sector aligned'
# A GUID table's root ends before the backup array and header, the disk's
# last 33 sectors.
start=266240
count=$((sectors - start))
[ "$table" = mbr ] || count=$((count - 33))
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
"$repo/build/tools/inspect/growset" --save --lba "$start" "$seed" "$count" \
    > "$directory/grow-set.txt" || fail 'growset'
# growset's first range is the MBR, sector 0 in a whole 4096-byte block. A
# GUID table's grow also writes the primary table, sectors 1 to 33, and the
# backup at the disk's end; the first range becomes sectors 0 to 39, the
# table in whole blocks, and the backup is the last.
awk -v table="$table" -v sectors="$sectors" '
    $1 != "range" { next }
    table == "gpt" && $2 == 0 { $3 = 40 }
    { print $2, $3 }
    END { if (table == "gpt") print sectors - 33, 33 }
' "$directory/grow-set.txt" > "$directory/ranges.txt"
cp "$repo/tools/dev/machine_grow.sh" "$directory/machine_grow.sh"
printf '%s  seed.ext2\n' "$seed_hash" > "$directory/seed.sha256"
for pair in utils/growboot:/bin/growboot utils/growset:/bin/growset utils/growroot:/bin/growroot utils/reflash:/bin/reflash \
    utils/diskctl:/bin/diskctl utils/sbin/bootctl:/bin/bootctl \
    vendor/e2fsprogs/e2fsck:/sbin/e2fsck vendor/e2fsprogs/resize2fs:/sbin/resize2fs; do
    sum_line=$(shasum -a 256 "$ARCH_BUILD/${pair%%:*}") || fail 'recovery program checksum'
    printf '%s  %s\n' "${sum_line%% *}" "${pair#*:}"
done > "$directory/programs.sha256"
awk '{total += $2} END {printf "machine-grow-plan: result: %.0f sectors, %.0f bytes plus the boot partition\n", total, total * 512}' \
    "$directory/ranges.txt"
