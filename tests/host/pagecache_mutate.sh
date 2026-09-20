#!/bin/sh
# Compile page-cache writeback with and without its reservation checks.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pagecache-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/mm/pagecache.c" "$work/pagecache.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/pagecache.saved" "$work/sys/mm/pagecache.c"
    touch "$work/sys/mm/pagecache.c"
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
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wno-empty-body \
        -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        tests/host/pagecache_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/mm/pagecache.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "pagecache-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "pagecache-mutation: result: $phase status $status"
}

echo "pagecache-mutation: logs: $work"
run_phase fixed 0 'pagecache-hosttest: result: pass: 0 failures'
for fault in pin busy; do
    python3 - "$work/sys/mm/pagecache.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('static int pc_writeback(')
end = source.index('\n/**', start)
body = source[start:end]
changes = {
    'pin': ('    e->pinned = 1;\n', ''),
    'busy': ('    if (e->pinned) {\n        return -1;\n    }\n', ''),
}
old, new = changes[sys.argv[2]]
if body.count(old) != 1:
    raise SystemExit('pagecache-mutation: source pattern is not unique')
path.write_text(source[:start] + body.replace(old, new) + source[end:])
PY
    case $fault in
        pin) message='pagecache-hosttest: failed: pc_test_page(0) == page' ;;
        busy) message='pagecache-hosttest: failed: fake_attempts[0] == 1' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'pagecache-hosttest: result: pass: 0 failures'
    cmp "$work/pagecache.saved" "$work/sys/mm/pagecache.c"
done
