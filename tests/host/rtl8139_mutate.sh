#!/bin/sh
# Compile each phase from a private tree; no kernel image is replaced.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-rtl8139.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
source="$work/sys/network/driver/rtl8139.c"
cp "$source" "$work/rtl8139.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/rtl8139.saved" "$source"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" \
        tests/host/rtl8139_hosttest.c \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "rtl8139-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'rtl8139-test: fail: repeated overflows produce one warning per boot' \
                "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "rtl8139-mutation: result: $phase status $status"
}

echo "rtl8139-mutation: logs: $work"
run_phase fixed
[ "$(grep -c 'if (!rx_overflow_logged)' "$source")" -eq 1 ]
sed 's/if (!rx_overflow_logged)/if (1)/' "$work/rtl8139.saved" > "$source"
run_phase mutated
cleanup
run_phase restored
cmp "$work/rtl8139.saved" "$source"
