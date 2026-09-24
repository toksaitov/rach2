#!/bin/sh
# Compile page-cache writeback and the syscall with their busy checks removed.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || exit 2
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pagecache-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/mm/pagecache.c" "$work/pagecache.saved"
cp "$work/sys/syscall_fs.c" "$work/syscall.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/pagecache.saved" "$work/sys/mm/pagecache.c"
    cp "$work/syscall.saved" "$work/sys/syscall_fs.c"
    touch "$work/sys/mm/pagecache.c" "$work/sys/syscall_fs.c"
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
    python3 - "$work" <<'PYEXTRACT'
from pathlib import Path
import re
import sys

work = Path(sys.argv[1])
source = (work / "sys/syscall_fs.c").read_text()
functions = re.findall(r"^int32_t sys_sync\(isr_params_t \*params\)\n\{.*?^\}",
                       source, re.M | re.S)
if len(functions) != 1:
    raise SystemExit("pagecache-mutation: sys_sync is not unique")
(work / "syscall_sync.inc").write_text(functions[0] + "\n")
PYEXTRACT
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -DPAGECACHE_SYNC_HOSTTEST -g -O1 -ffreestanding -Wno-empty-body \
        -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        tests/host/pagecache_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/mm/pagecache.c" "$work/sys/syscall_fs.c" "$work/syscall_sync.inc" "$binary" >"$binary.cksum"
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
[ "$#" -eq 0 ] || exit 0
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

python3 - "$work/sys/syscall_fs.c" <<'PYMUTATE'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
source = path.read_text()
source, count = re.subn(
    r"^int32_t sys_sync\(isr_params_t \*params\)\n\{.*?^\}",
    "int32_t sys_sync(isr_params_t *params)\n{\n"
    "    (void)params;\n    vfs_sync_all();\n    pc_sync();\n    return 0;\n}",
    source, flags=re.M | re.S)
if count != 1:
    raise SystemExit("pagecache-mutation: sys_sync is not unique")
path.write_text(source)
PYMUTATE
run_phase sync-mutated 1 'pagecache-hosttest: failed: fake_disk[0][0] == 0xE2u'
cleanup
run_phase sync-restored 0 'pagecache-hosttest: result: pass: 0 failures'
cmp "$work/syscall.saved" "$work/sys/syscall_fs.c"
