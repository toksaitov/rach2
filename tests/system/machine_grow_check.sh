#!/bin/sh
# shellcheck disable=SC2034,SC2016 # Extracted functions use these variables.
# Exercise the machine's checks with real seed files and complete boot reads.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-grow-check.XXXXXX")
echo "machine-grow-check: logs: $work"
fail() { echo "machine-grow-check: failed: $*" >&2; exit 1; }
[ "$#" = 0 ] || [ "$*" = --mutate ] || [ "$*" = --texts ] || fail 'usage: machine_grow_check.sh [--mutate | --texts]'
cp "$repo/tools/dev/machine_grow.sh" "$work/original.sh"
cp "$work/original.sh" "$work/source.sh"

# mutate_source MUTATION: $work/source.sh is $work/original.sh with the
# check MUTATION names taken out.
mutate_source()
{
    case "$1" in
        ranges) pattern='cmp "$work/ranges.txt" "$work/expected-ranges.txt"' ;;
        boot) pattern='[ "$actual_boot" = "$expected_boot" ]' ;;
    esac
    awk -v pattern="$pattern" 'index($0, pattern) {print "    :"; next} {print}' \
        "$work/original.sh" > "$work/source.sh"
}

if [ "$*" = --texts ]; then
    for mutation in ranges boot; do
        mutate_source "$mutation"
        if cmp -s "$work/original.sh" "$work/source.sh"; then
            echo "machine-grow-check: the $mutation fault is not in tools/dev/machine_grow.sh" >&2
            exit 1
        fi
    done
    trap - EXIT
    rm -rf "$work"
    echo 'machine-grow-check: texts: every fault is once in its source'
    exit 0
fi
# shellcheck disable=SC2329
cleanup() { cp "$work/original.sh" "$work/source.sh"; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cc -O2 -Wall -Wextra -o "$work/growset" "$repo/tools/inspect/growset.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/reflash" \
    "$repo/utils/reflash.c" "$repo/utils/sectorcopy.c" "$repo/utils/sha256.c"
# shellcheck disable=SC2329
reflash() { "$work/reflash" "$@"; }
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
for profile in small samsung; do
    mkdir "$work/$profile"
    if [ "$profile" = small ]; then size=8; group=1024; else size=1024; group=32768; fi
    dd if=/dev/zero of="$work/$profile/seed.ext2" bs=1048576 count=0 seek="$size" 2>"$work/$profile/dd.log"
    E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g "$group" -I 128 \
        -O sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
        "$work/$profile/seed.ext2" >"$work/$profile/mkfs.log" 2>&1
done
mkdir "$work/saved"
# The last boot byte is nonzero, so a read of only the header cannot pass.
dd if=/dev/zero of="$work/disk" bs=1048576 count=65 2>"$work/disk.log"
printf x | dd of="$work/disk" bs=1 seek=68157439 conv=notrunc 2>>"$work/disk.log"
dd if="$work/disk" of="$work/saved/boot.original" bs=1048576 skip=1 count=64 2>>"$work/disk.log"

extract()
{
    # Extract complete production functions; adapt only the guest binary path
    # to its native build. The extent reader is the real reflash verifier.
    awk '
        /^(hash_file|hash_slice|check_plan|check_boot_copy)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$work/source.sh" > "$work/functions.sh"
    sed "s|/bin/growset|$work/growset|g" "$work/functions.sh" > "$work/checks.sh"
}

rows()
{
    phase=$1
    extract
    (
        # shellcheck disable=SC1091
        . "$work/checks.sh"
        start=266240
        for profile in small samsung; do
            store="$work/$profile"
            if [ "$profile" = small ]; then count=131072; else count=488130928; fi
            seed=$(( $(wc -c < "$store/seed.ext2") / 512 ))
            seed_hash=$(hash_file "$store/seed.ext2")
            "$work/growset" --save --lba "$start" "$store/seed.ext2" "$count" > "$work/generated"
            awk '$1 == "range" {print $2, $3}' "$work/generated" > "$work/ranges.txt"
            check_plan
            cp "$work/ranges.txt" "$work/$profile.ranges"
            awk 'NR == FNR {n++; next} FNR == n {$1 += $2} {print}' \
                "$work/$profile.ranges" "$work/$profile.ranges" > "$work/ranges.txt"
            # This mutation keeps the byte total, seed coverage and bounds.
            if (check_plan) >"$work/$phase-$profile.log" 2>&1; then
                fail "$profile omitted range accepted"
            fi
            grep -q 'ranges differ from the seed and target geometry' "$work/$phase-$profile.log" || fail 'unexpected range failure'
        done
        saved="$work/saved"
        disk="$work/disk"
        cp "$saved/boot.original" "$saved/boot.img"
        check_boot_copy
        printf y | dd of="$saved/boot.img" bs=1 seek=67108863 conv=notrunc 2>"$work/corrupt.log"
        # Authenticate the corrupted copy just as the old save did.
        (cd "$saved" && sha256sum boot.img > SHA256SUMS && sha256sum -c SHA256SUMS)
        if (check_boot_copy) >"$work/$phase-boot.log" 2>&1; then
            fail 'same-length boot corruption accepted'
        fi
        grep -q 'the boot backup differs from the disk' "$work/$phase-boot.log" || fail 'unexpected boot failure'
    ) >"$work/$phase.log" 2>&1
}
rows fixed || { cat "$work/fixed.log" >&2; fail 'fixed checks'; }
echo 'machine-grow-check: fixed: both geometries and the complete boot copy passed'
if [ "$#" = 1 ]; then
    for mutation in ranges boot; do
        mutate_source "$mutation"
        if rows "$mutation"; then fail "$mutation mutation passed"; fi
        case "$mutation" in
            ranges) diagnostic='small omitted range accepted' ;;
            boot) diagnostic='same-length boot corruption accepted' ;;
        esac
        grep -q "$diagnostic" "$work/$mutation.log" || fail "$mutation did not fail its assertion"
        echo "machine-grow-check: mutation: $mutation detected"
        cp "$work/original.sh" "$work/source.sh"
        rows "$mutation-restored" || { cat "$work/$mutation-restored.log" >&2; fail 'restored checks'; }
        echo "machine-grow-check: restored: $mutation checks passed"
    done
fi
