#!/bin/sh
# Prove returned-write durability with private ATA driver mutations.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('static int ata_write_on(')
end = source.index('\n}', start) + 2
body = source[start:end]
command = 'if (!ata_command(ch, d->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH))'
if body.count(command) != 1:
    raise SystemExit('ata-durable-mutation: flush command pattern is not unique')
if sys.argv[2] == 'command':
    body = body.replace(command, 'if (0)')
else:
    split = body.index(command)
    prefix, suffix = body[:split], body[split:]
    wait = 'if (ata_wait_ready(ch) != 0)'
    if suffix.count(wait) != 1:
        raise SystemExit('ata-durable-mutation: flush wait pattern is not unique')
    body = prefix + suffix.replace(wait, 'if (0)')
path.write_text(source[:start] + body + source[end:])
PY
}

faults='command wait'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/block/ata.c "$fault" >/dev/null || exit 1
    done
    echo 'ata-durable-proof: texts: every fault is once in its source'
    exit 0
fi

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ata-durable-proof.XXXXXX")
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/ata_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/block/ata.c"
cp "$source" "$work/source.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
    cp "$work/source.saved" "$source"
    touch "$source"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$work/tests/host/ata_hosttest.c" \
        "$work/rach2/sys/lib/kstring.c" "$work/rach2/sys/lib/kctypes.c" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'setpgrp(0, 0) or die "ata-durable-mutation: setpgrp: $!\n";
             alarm 60; exec @ARGV or die "ata-durable-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    cleanup
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "ata-durable-mutation: result: $phase status $status"
}

echo "ata-durable-mutation: logs: $work"
run_phase fixed 0 'ata-test: OK'
for fault in $faults; do
    apply_fault "$source" "$fault"
    run_phase "$fault-mutated" 1 'ata-test: fail: a returned write is durable before power loss'
    cmp "$source" "$work/source.saved"
    run_phase "$fault-restored" 0 'ata-test: OK'
    cmp "$source" "$work/source.saved"
done
echo "ata-durable-mutation: result: both compiled faults failed and restored sources passed"
