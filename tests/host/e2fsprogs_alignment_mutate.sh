#!/bin/sh
# Prove the alignment tests by restoring the old buffer-address check.
# Run from any directory. Logs and the saved source go to a temporary one.

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
source=vendor/e2fsprogs/e2fsprogs-1.47.4/lib/ext2fs/unix_io.c
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-alignment.XXXXXX")
child=
mutated=0

# The test is one process. Stop and reap it before restoring its source.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ "$mutated" -eq 1 ]; then
        cp -p "$work/unix_io.c.saved" "$source"
        touch "$source"
        cmp "$work/unix_io.c.saved" "$source"
    fi
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

command -v perl >/dev/null
cp -p "$source" "$work/unix_io.c.saved"
original='#define BUFFER_ALIGNED(buf, align) 1'
if [ "$(grep -Fxc "$original" "$source")" != 1 ]; then
    echo "alignment-mutation: expected one rach2 buffer alignment definition" >&2
    exit 1
fi

# Compile fresh binaries in each phase with the host runner's flags.
run_phase()
{
    phase=$1
    for variant in e2fsprogs_alignment e2fsprogs_alignment64; do
        test_source="tests/host/${variant}_hosttest.c"
        binary="$work/$phase-$variant"
        log="$binary.rlog"
        extra=$(sed -n 's/^.*host-cflags: *//p' "$test_source")
        # shellcheck disable=SC2086
        if ! ${CC:-cc} -std=c99 -g -O1 -Wno-empty-body -ffreestanding \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils \
            -I krffs -I tests/host/compat $extra \
            -o "$binary" "$test_source" >"$binary.clog" 2>&1; then
            cat "$binary.clog" >&2
            echo "alignment-mutation: compile failed: $phase $variant" >&2
            exit 1
        fi
        # alarm survives exec and bounds the single test process to ten seconds.
        perl -e 'alarm 10; exec @ARGV or die "alignment-mutation: exec: $!\n"' \
            "$binary" >"$log" 2>&1 &
        child=$!
        status=0
        wait "$child" || status=$?
        child=
        if [ "$phase" = mutated ]; then
            failures=$(grep -c ': fail:' "$log" || :)
            misaligned=$(grep -c ': fail: misaligned ' "$log" || :)
            if [ "$status" -ne 14 ] || [ "$failures" -ne 14 ] || [ "$misaligned" -ne 14 ] ||
                ! grep -Fq ': fail: misaligned read uses one whole call' "$log" ||
                ! grep -Fq ': fail: misaligned write uses one whole call' "$log"; then
                cat "$log" >&2
                echo "alignment-mutation: unexpected mutation result: $variant status $status" >&2
                exit 1
            fi
            echo "alignment-mutation: result: mutated $variant failed 14 misaligned checks as expected"
        elif [ "$status" -eq 0 ]; then
            echo "alignment-mutation: result: $phase $variant passed"
        else
            cat "$log" >&2
            echo "alignment-mutation: test failed: $phase $variant status $status" >&2
            exit 1
        fi
    done
}

echo "alignment-mutation: logs: $work"
run_phase patched
sed 's/^#define BUFFER_ALIGNED(buf, align) 1$/#define BUFFER_ALIGNED(buf, align) IS_ALIGNED(buf, align)/' \
    "$work/unix_io.c.saved" >"$work/unix_io.c.mutated"
mutated=1
cat "$work/unix_io.c.mutated" >"$source"
touch "$source"
run_phase mutated
cleanup
mutated=0
run_phase restored
cmp "$work/unix_io.c.saved" "$source"
echo "alignment-mutation: result: source restored byte for byte"
