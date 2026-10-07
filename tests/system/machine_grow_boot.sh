#!/bin/sh
# Run the helper's boot preparation and repair over a private FAT image.
# Extracted functions use and assign these variables.
# shellcheck disable=SC2034,SC2016,SC2154
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-grow-boot.XXXXXX")
echo "machine-grow-boot: logs: $work"
fail() { echo "machine-grow-boot: failed: $*" >&2; exit 1; }
[ "$#" = 0 ] || [ "$*" = --mutate ] || [ "$*" = --texts ] || fail 'usage: machine_grow_boot.sh [--mutate | --texts]'
cp "$repo/tools/dev/machine_grow.sh" "$work/original.sh"
cp "$work/original.sh" "$work/source.sh"

# mutate_source MUTATION: $work/source.sh is $work/original.sh with
# MUTATION in it, and `diagnostic` the words its failing row prints.
mutate_source()
{
    case "$1" in
        rewrite)
            sed 's/if \[ "$wanted" = "$current" \]; then/if false; then/' \
                "$work/original.sh" > "$work/source.sh"
            diagnostic='verified recovery was rewritten'
            ;;
        repair)
            sed 's/| reflash "$disk" "$((1048576 + at))" "$amount"; then/| cat > \/dev\/null; then/' \
                "$work/original.sh" > "$work/source.sh"
            diagnostic='repair did not reach the torn write'
            ;;
    esac
}

if [ "$*" = --texts ]; then
    for mutation in rewrite repair; do
        mutate_source "$mutation"
        if cmp -s "$work/original.sh" "$work/source.sh"; then
            echo "machine-grow-boot: the $mutation fault is not in tools/dev/machine_grow.sh" >&2
            exit 1
        fi
    done
    trap - EXIT
    rm -rf "$work"
    echo 'machine-grow-boot: texts: every fault is once in its source'
    exit 0
fi
# shellcheck disable=SC2329
cleanup() { cp "$work/original.sh" "$work/source.sh"; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'rach2.slot=recovery\n' > "$work/cmdline"
sed "s|/proc/cmdline|$work/cmdline|g" "$repo/utils/bootctl.c" > "$work/bootctl.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/bootctl" "$work/bootctl.c" \
    "$repo/utils/bootslot.c" "$repo/utils/bootverb.c" "$repo/utils/grubslot.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/growboot" "$repo/utils/growboot.c" "$repo/utils/bootslot.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$work/reflash" \
    "$repo/utils/reflash.c" "$repo/utils/sectorcopy.c" "$repo/utils/sha256.c"
cc -O2 -Wall -Wextra -o "$work/mkfatfixture" "$repo/tests/host/mkfatfixture.c"
"$work/mkfatfixture" "$work/boot.original" --cluster8
# Pad the independent fixture to the machine helper's complete boot extent.
python3 - "$work/boot.original" <<'PY'
import sys
with open(sys.argv[1], 'r+b') as volume:
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

extract()
{
    awk '
        /^(hash_slice|prepare_boot|keep_recovery)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$work/source.sh" > "$work/extracted.sh"
    sed "s|/bin/growboot|$work/growboot|g" "$work/extracted.sh" > "$work/functions.sh"
}

# These adapters replace only disk I/O and the guest command-line pathname.
# FAT parsing, environment editing and the shell's decisions are production.
# shellcheck disable=SC2329
dd()
{
    case " $* " in
        *' iflag=skip_bytes,count_bytes '*) python3 "$work/io.py" dd "$work" "$@" ;;
        *) command dd "$@" ;;
    esac
}
# shellcheck disable=SC2329
read_boot()
{
    command dd if="$disk" of="$work/current-boot.img" bs=1048576 skip=1 count=64 2>"$work/read.log"
    [ "$(wc -c < "$work/current-boot.img" | tr -d ' ')" = 67108864 ] || fail 'short boot read'
}
# shellcheck disable=SC2329
reflash()
{
    if [ "$1" = --verify ]; then
        "$work/reflash" "$@"
    else
        python3 "$work/io.py" write "$work" "$@"
    fi
}
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

rows()
{
    phase=$1
    extract
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
    ) >"$work/$phase.log" 2>&1
}
rows fixed || { cat "$work/fixed.log" >&2; fail 'fixed boot checks'; }
echo 'machine-grow-boot: fixed: unchanged recovery, invalid header, torn repair and retry passed'
if [ "$#" = 1 ]; then
    for mutation in rewrite repair; do
        mutate_source "$mutation"
        if rows "$mutation"; then fail "$mutation mutation passed"; fi
        grep -q "$diagnostic" "$work/$mutation.log" || fail "$mutation did not fail its assertion"
        echo "machine-grow-boot: mutation: $mutation detected"
        cp "$work/original.sh" "$work/source.sh"
        rows "$mutation-restored" || { cat "$work/$mutation-restored.log" >&2; fail 'restored boot checks'; }
        echo "machine-grow-boot: restored: $mutation checks passed"
    done
fi
