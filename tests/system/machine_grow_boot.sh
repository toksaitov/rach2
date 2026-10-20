#!/bin/sh
# machine_grow_boot.sh [SCRIPT]: check prepare_boot and keep_recovery of
# tools/dev/machine_grow.sh, or of SCRIPT, a copy of it, on private FAT
# images, with bootctl, growboot and reflash built natively.
#
# Every row runs on two boot partitions: one at eight sectors to a cluster,
# as an MBR disk's is, and one at one sector to a cluster, as the EFI system
# partition of a GUID table is, whose environment is two clusters.
# A boot partition whose environment already starts recovery must not be
# written. One whose environment header is zeroed must be repaired with its
# 1024 bytes alone, from the copy prepare_boot made; a repair torn after one
# sector must fail, and the retry must finish it. An environment that starts
# the candidate must be rewritten in place, and growboot must refuse to
# write its input. tests/proofs/machine-grow-boot.json gives a private copy
# of the script in which keep_recovery rewrites an environment that is
# already right, and one in which the repair writes nothing, and requires
# each to fail its row.
# Extracted functions use and assign these variables.
# shellcheck disable=SC2034,SC2016,SC2154
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" machine-grow-boot "$repo/tests/system/machine_grow_boot.sh" "$@"
work=
# Remove the scratch directory, whose disk and boot images take some
# 300 MiB; a failure prints the rows' log first.
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# Report $* and exit 1.
fail() { echo "machine-grow-boot: failed: $*" >&2; exit 1; }
[ "$#" -le 1 ] || fail 'usage: machine_grow_boot.sh [SCRIPT]'
mkdir -p "$repo/build/tests"
work=$(mktemp -d "$repo/build/tests/machine-grow-boot.XXXXXX")
cp "${1:-$repo/tools/dev/machine_grow.sh}" "$work/source.sh"

printf 'rach2.slot=recovery\n' > "$work/cmdline"
sed "s|/proc/cmdline|$work/cmdline|g" "$repo/utils/bootctl.c" > "$work/bootctl.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/bootctl" "$work/bootctl.c" \
    "$repo/utils/bootslot.c" "$repo/utils/bootverb.c" "$repo/utils/grubslot.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/growboot" "$repo/utils/growboot.c" "$repo/utils/bootslot.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/reflash" \
    "$repo/utils/reflash.c" "$repo/utils/sectorcopy.c" "$repo/utils/sha256.c"
cc -O2 -Wall -Wextra -o "$work/mkfatfixture" "$repo/tests/host/mkfatfixture.c"
"$work/mkfatfixture" "$work/boot.cluster8" --cluster8
"$work/mkfatfixture" "$work/boot.cluster1"
# Pad the independent fixtures to the machine helper's complete boot extent.
python3 - "$work/boot.cluster8" "$work/boot.cluster1" <<'PY'
import sys
for path in sys.argv[1:]:
    with open(path, 'r+b') as volume:
        volume.truncate(67108864)
PY
mkdir "$work/saved"
cat > "$work/io.py" <<'PY'
import os
import sys
mode, root, *args = sys.argv[1:]
if mode == 'dd':
    fields = dict(arg.split('=', 1) for arg in args)
    dst = open(fields['of'], 'wb') if 'of' in fields else sys.stdout.buffer
    with open(fields['if'], 'rb') as src:
        src.seek(int(fields['skip']))
        left = int(fields['count'])
        while left:
            data = src.read(min(left, 1048576))
            if not data:
                sys.exit(1)
            dst.write(data)
            left -= len(data)
else:
    path, at, length = args
    at, length = int(at), int(length)
    assert path == root + '/disk'
    assert at >= 1048576 and at + length <= 68157440
    data = sys.stdin.buffer.read(length + 1)
    assert len(data) == length
    tear = os.getenv('GROW_BOOT_TEAR') == '1'
    if tear:
        assert length == 1024
        data = data[:512]
    with open(root + '/writes', 'a') as log:
        log.write(f'{at} {len(data)}\n')
    with open(path, 'r+b') as dst:
        dst.seek(at)
        dst.write(data)
        dst.flush()
        os.fsync(dst.fileno())
    if tear:
        sys.exit(1)
PY

# Copy hash_slice, prepare_boot and keep_recovery whole from source.sh into
# functions.sh, with the guest's /bin/growboot replaced by the native build.
extract()
{
    awk '
        /^(hash_slice|prepare_boot|keep_recovery)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$work/source.sh" > "$work/extracted.sh"
    sed "s|/bin/growboot|$work/growboot|g" "$work/extracted.sh" > "$work/functions.sh"
}

# The functions below replace only the disk's reads and writes and the path
# of the guest's command line; the FAT parsing, the editing of the
# environment and the decisions are the script's own. dd ARGS: io.py for a
# read addressed in bytes, the host's dd otherwise.
# shellcheck disable=SC2329
dd()
{
    case " $* " in
        *' iflag=skip_bytes,count_bytes '*) python3 "$work/io.py" dd "$work" "$@" ;;
        *) command dd "$@" ;;
    esac
}
# Copy the disk's 64 MiB boot partition to current-boot.img.
# shellcheck disable=SC2329
read_boot()
{
    command dd if="$disk" of="$work/current-boot.img" bs=1048576 skip=1 count=64 2>"$work/read.log"
    [ "$(wc -c < "$work/current-boot.img" | tr -d ' ')" = 67108864 ] || fail 'short boot read'
}
# reflash ARGS: the native reflash for --verify, and otherwise a write
# through io.py, which logs it and, with GROW_BOOT_TEAR=1, writes only its
# first sector and fails.
# shellcheck disable=SC2329
reflash()
{
    if [ "$1" = --verify ]; then
        "$work/reflash" "$@"
    else
        python3 "$work/io.py" write "$work" "$@"
    fi
}
# bootctl --target DEVICE ARGS: the native bootctl, on a copy of the boot
# partition read now when DEVICE is the disk's first partition.
# shellcheck disable=SC2329
bootctl()
{
    if [ "$2" = "${disk}p1" ]; then
        read_boot
        shift 2
        "$work/bootctl" --target "$work/current-boot.img" "$@"
    else
        "$work/bootctl" "$@"
    fi
}

# rows PHASE: run the boot rows on the functions of source.sh for each
# boot partition, with the output in PHASE.log; status 1 when a row fails.
rows()
{
    phase=$1
    extract
    : > "$work/$phase.log"
    for geometry in cluster8 cluster1; do
        cp "$work/boot.$geometry" "$work/boot.original"
        echo "machine-grow-boot: $geometry" >> "$work/$phase.log"
        geometry_rows || return 1
    done
}

# geometry_rows: the rows of `rows` on boot.original, with the output
# appended to PHASE.log.
geometry_rows()
{
    (
        # shellcheck disable=SC1091
        . "$work/functions.sh"
        saved="$work/saved"
        disk="$work/disk"
        cp "$work/boot.original" "$saved/boot.img"
        rm -f "$work/recovery.env"
        prepare_boot
        command dd if=/dev/zero of="$disk" bs=1048576 count=1 2>"$work/create.log"
        cat "$work/boot.original" >> "$disk"
        command dd if="$work/recovery.env" of="$disk" bs=512 seek="$((2048 + env_sector))" \
            count=2 conv=notrunc 2>"$work/initial-env.log"
        printf '# GRUB Environment Block\n# preserve this comment\nsaved_entry=recovery\n' > "$work/good-env"
        n=$(wc -c < "$work/good-env" | tr -d ' ')
        awk -v n="$((1024 - n))" 'BEGIN {for (i = 0; i < n; i++) printf "#"}' >> "$work/good-env"
        command dd if="$work/good-env" of="$disk" bs=512 seek="$((2048 + env_sector))" \
            count=2 conv=notrunc 2>"$work/good-env.log"
        : > "$work/writes"
        keep_recovery
        [ ! -s "$work/writes" ] || fail 'verified recovery was rewritten'
        # Damage the header, not just a value bootctl still accepts.
        command dd if=/dev/zero of="$disk" bs=512 seek="$((2048 + env_sector))" \
            count=2 conv=notrunc 2>"$work/damage.log"
        if bootctl --target "${disk}p1" env saved_entry recovery >"$work/unparseable.log" 2>&1; then
            fail 'the damaged header was parseable'
        fi
        grep -q "the environment block header is not GRUB's" "$work/unparseable.log" || fail 'wrong header failure'
        rm "$work/recovery.env"
        prepare_boot
        if (export GROW_BOOT_TEAR=1; keep_recovery) >"$work/$phase-torn.log" 2>&1; then
            fail 'the torn repair succeeded'
        fi
        grep -q 'repairing boot environment' "$work/$phase-torn.log" || fail 'repair did not reach the torn write'
        printf '%s 512\n' "$((1048576 + env_offset))" > "$work/expected-writes"
        cmp "$work/expected-writes" "$work/writes" || fail 'the repair did not stop after one sector'
        rm "$work/recovery.env"
        prepare_boot
        keep_recovery
        printf '%s 1024\n' "$((1048576 + env_offset))" >> "$work/expected-writes"
        cmp "$work/expected-writes" "$work/writes" || fail 'repair wrote outside the environment'
        read_boot
        cp "$work/boot.original" "$work/expected-boot.img"
        command dd if="$work/recovery.env" of="$work/expected-boot.img" bs=512 seek="$env_sector" \
            count=2 conv=notrunc 2>"$work/expected-env.log"
        cmp "$work/expected-boot.img" "$work/current-boot.img" || fail 'repair differs from the prepared backup'
        : > "$work/writes"
        keep_recovery
        [ ! -s "$work/writes" ] || fail 'verified recovery was rewritten on retry'
        # A valid candidate default also changes through the prepared copy.
        command dd if="$work/boot.original" of="$disk" bs=512 skip="$env_sector" \
            seek="$((2048 + env_sector))" count=2 conv=notrunc 2>"$work/candidate.log"
        keep_recovery
        printf '%s 1024\n' "$((1048576 + env_offset))" > "$work/candidate-writes"
        cmp "$work/candidate-writes" "$work/writes" || fail 'candidate conversion wrote outside the environment'
        : > "$work/writes"
        if "$work/growboot" "$saved/boot.img" "$saved/boot.img"; then
            fail 'growboot allowed overwriting its input'
        fi
        cmp "$work/boot.original" "$saved/boot.img" || fail 'the boot backup was changed'
    ) >>"$work/$phase.log" 2>&1
}
rows fixed || { cat "$work/fixed.log" >&2; fail 'fixed boot checks'; }
echo 'machine-grow-boot: fixed: unchanged recovery, invalid header, torn repair and retry passed on both geometries'
