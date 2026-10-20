#!/bin/sh
# Compile and run ata_hosttest.c for the diskrate suite. Its two write-count
# rows are proved by tests/proofs/ata-rate.json.
#
# Usage: ata_rate_test.sh
set -eu
[ "$#" -eq 0 ] || {
    echo "ata-rate-test: usage: ata_rate_test.sh" >&2
    exit 2
}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=
child=
trap '[ -z "$child" ] || kill -TERM "$child" 2>/dev/null; [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/ata-rate.XXXXXX")
binary="$work/ata_hosttest"
if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I rach2/sys -I rach2/sys/lib tests/host/ata_hosttest.c rach2/sys/lib/kstring.c \
    rach2/sys/lib/kctypes.c -o "$binary" >"$work/compile.log" 2>&1; then
    cat "$work/compile.log" >&2
    exit 1
fi
perl -e 'alarm 30; exec @ARGV or die "ata-rate-test: exec: $!\n"' \
    "$binary" >"$work/test.log" 2>&1 &
child=$!
status=0
wait "$child" || status=$?
child=
cat "$work/test.log"
echo "ata-rate-test: result: status $status"
[ "$status" -eq 0 ]
