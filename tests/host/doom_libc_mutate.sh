#!/bin/sh
# Compile Doom's C library with a freestanding fixture for both x86 ABIs.
# --check runs the fixture; --texts checks mutation patterns without builds.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ROOT FAULT: change one private source, or check its pattern.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
root = Path(sys.argv[1])
changes = {
    'limits': ('include/limits.h', '#define LONG_MAX    __LONG_MAX__',
               '#define LONG_MAX    2147483647L'),
    'types': ('include/sys/types.h', 'typedef long off_t;', 'typedef int32_t off_t;'),
    'buffer-format': ('crt0.c', 'int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)', ''),
    'print-format': ('crt0.c', 'int vprintf(const char *fmt, va_list ap)', ''),
    'scan': ('crt0.c', "base = 16;\n            str += 2;", "base = 10;\n            str += 2;"),
    'sort': ('include/stdlib.h', 'if (nmemb < 2 || size == 0) return;', 'if (size == 0) return;'),
    'cwd': ('include/unistd.h', 'if (!buf || size < 2) return NULL;', 'if (!buf || size < 1) return NULL;'),
    'update': ('crt0.c', "if (strchr(mode, '+')) flags = (flags & ~3) | O_RDWR;", ''),
}
name, old, new = changes[sys.argv[2]]
p = root / name
s = p.read_text()
if s.count(old) != 1:
    raise SystemExit('doom-libc-mutation: source pattern is not unique')
if sys.argv[2] in ('buffer-format', 'print-format'):
    start = s.index(old)
    end = s.index('\nint ', start + len(old))
    body = s[start:end]
    target = "if (*p == 'l') { is_long = 2; p++; }"
    if body.count(target) != 1:
        raise SystemExit('doom-libc-mutation: length pattern is not unique')
    s = s[:start] + body.replace(target, "if (*p == 'l') p++;") + s[end:]
else:
    s = s.replace(old, new)
if not os.environ.get('RACH2_FAULT_TEXTS'):
    p.write_text(s)
PY
}

faults='limits types buffer-format print-format scan sort cwd update'
case ${1:-} in
    --texts)
        for fault in $faults; do
            RACH2_FAULT_TEXTS=1 apply_fault ports/doom "$fault"
        done
        echo 'doom-libc-mutation: texts: every fault is once in its source'
        exit 0 ;;
    --check|'') ;;
    *) echo 'doom-libc-mutation: usage: doom_libc_mutate.sh [--check | --texts]' >&2; exit 2 ;;
esac
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" doom-libc-mutation "$repo/tests/host/doom_libc_mutate.sh" "$@"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/doom-libc-proof.XXXXXX")
mkdir "$work/original" "$work/source"
cp -R ports/doom/include ports/doom/crt0.c "$work/original/"
child=

# restore: replace the private sources and make their timestamps current.
restore()
{
    cp -R "$work/original/." "$work/source/"
    touch "$work/source/crt0.c"
}

# cleanup: stop the fixture before restoring the private sources.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase NAME EXPECTED: require both ABIs to pass, or one to fail.
run_phase()
{
    phase=$1
    expected=$2
    failed=0
    matched=0
    case $phase in
        limits-mutated) message="long maximum" ;;
        types-mutated) message="off_t width" ;;
        buffer-format-mutated) message="64-bit macros" ;;
        scan-mutated) message="automatic hex base" ;;
        sort-mutated) message="empty sort calls comparator" ;;
        cwd-mutated) message="cwd size one" ;;
        update-mutated) message="read w+" ;;
        *) message= ;;
    esac
    for arch in i686 x86_64; do
        binary=$work/$phase-$arch
        "${arch}-elf-gcc" -std=gnu99 -Os -ffreestanding -fno-pie -no-pie \
            -nostdlib -static -Wl,--build-id=none -I "$work/source/include" \
            "$work/source/crt0.c" tests/host/fixtures/doom_libc.c -o "$binary" \
            >"$binary.compile.log" 2>&1 || {
                cat "$binary.compile.log" >&2; exit 1;
            }
        cksum "$work/source/crt0.c" "$binary" >"$binary.cksum"
        perl -e 'alarm 10; exec @ARGV or die "doom-libc-mutation: exec: $!\n"' \
            "$binary" "$binary.data" >"$binary.log" 2>&1 &
        child=$!
        status=0
        wait "$child" || status=$?
        child=
        cat "$binary.log"
        [ "$status" -le 1 ] || {
            echo "doom-libc-mutation: fixture ended with status $status" >&2; exit 1;
        }
        if [ -n "$message" ] && grep -Fq "doom-libc-test: fail: $message" "$binary.log"; then
            matched=1
        fi
        if [ "$status" -ne 0 ] || ! grep -Fxq \
            'doom-libc-format: -9223372036854775808 18446744073709551615 ffffffffffffffff 37' \
            "$binary.log"; then
            failed=1
        fi
    done
    [ "$failed" -eq "$expected" ] || {
        echo "doom-libc-mutation: unexpected result: $phase failed=$failed" >&2; exit 1;
    }
    [ -z "$message" ] || [ "$matched" -eq 1 ] || {
        echo "doom-libc-mutation: missing failure: $message" >&2; exit 1;
    }
    echo "doom-libc-mutation: result: $phase passed"
}

restore
echo "doom-libc-mutation: logs: $work"
run_phase fixed 0
[ "${1:-}" != --check ] || exit 0
for fault in $faults; do
    apply_fault "$work/source" "$fault"
    run_phase "$fault-mutated" 1
    restore
    run_phase "$fault-restored" 0
done
echo 'doom-libc-mutation: result: pass: eight compiled faults failed; restored runs passed'
