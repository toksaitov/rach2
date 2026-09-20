#!/bin/sh
# Compile fixed, faulty and restored boot-slot sources in a private directory.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/bootslot-proof.XXXXXX")
child=
cp utils/bootslot.c "$work/bootslot.saved"
cp "$work/bootslot.saved" "$work/bootslot.c"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/bootslot.saved" "$work/bootslot.c"
    touch "$work/bootslot.c"
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
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all -I utils \
        tests/host/bootslot_hosttest.c "$work/bootslot.c" -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum tests/host/bootslot_hosttest.c utils/bootslot.h \
        "$work/bootslot.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "bootslot-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "bootslot-mutation: result: $phase status $status"
}

echo "bootslot-mutation: logs: $work"
run_phase fixed 0 'bootslot-hosttest: result: pass:'
for fault in order bound; do
    python3 - "$work/bootslot.c" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
if sys.argv[2] == "order":
    start = source.index('    if (file_write_sector(bs, &bs->fallback, 0, head)')
    end = source.index('    return 0;', start)
    final = source[start:end]
    source = source[:start] + source[end:]
    before = '    for (s = 1; s < nsectors; s++) {\n'
    after = final + before
else:
    before = '''        if (p_memsz > 0xFFFFFFFFu - p_vaddr) {
            return fail(err, errlen, "a load leaves the 32-bit address space");
        }
'''
    after = ''
if source.count(before) != 1:
    raise SystemExit("bootslot-mutation: source pattern is not unique")
path.write_text(source.replace(before, after))
PY
    case $fault in
        order) message='install refuses the unverified fallback' ;;
        bound) message='load crossing 4 GiB is refused' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'bootslot-hosttest: result: pass:'
    cmp "$work/bootslot.saved" "$work/bootslot.c"
done
