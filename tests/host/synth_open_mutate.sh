#!/bin/sh
# Compile synthetic-file regressions before, during and after each fault.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/synth-open-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
for component in devfs procfs; do
    cp "$work/sys/fs/$component.c" "$work/$component.saved"
done

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    for component in devfs procfs; do
        cp "$work/$component.saved" "$work/sys/fs/$component.c"
        touch "$work/sys/fs/$component.c"
    done
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    test=$1
    phase=$2
    expected=$3
    message=$4
    binary="$work/$test-$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        "tests/host/${test}_hosttest.c" "$work/sys/lib/kstring.c" \
        "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/$test.c" "$binary" >"$binary.cksum"
    UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=1 perl -e 'alarm 10; exec @ARGV or die "synth-open-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "synth-open-mutation: result: $test $phase status $status"
}

# faults TEST: the faults compiled into the source of TEST, one at a time.
faults()
{
    case $1 in
        devfs) echo ptmx ;;
        procfs) echo terminator mem-hold mem-live ;;
    esac
}

echo "synth-open-mutation: logs: $work"
for test in devfs procfs; do
    run_phase "$test" fixed 0 "$test-hosttest: result: pass:"
    for fault in $(faults "$test"); do
        python3 - "$work/sys/fs/$test.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
if fault == 'ptmx':
    old = '    if (name_eq(name, "ptmx")) {\n        return pty_open_master(flags);\n    }\n'
    assert source.count(old) == 1
    source = source.replace(old, '')
    at = source.index('    dev = devfs_find(name);', source.index('int devfs_open('))
    source = source[:at] + old + source[at:]
elif fault == 'terminator':
    old = "    if (!t->nomem) {\n        t->buf[t->len] = '\\0';\n    }"
    assert source.count(old) == 1
    source = source.replace(old, "    t->buf[t->len] = '\\0';")
elif fault == 'mem-hold':
    old = '    mm_hold(p->mm);\n    f->mm = p->mm;\n'
    assert source.count(old) == 1
    source = source.replace(old, '    mm_get_live(p->mm);\n    f->mm = p->mm;\n')
else:
    old = '    return f->mm && mm_get_live(f->mm) ? f->mm : NULL;\n'
    assert source.count(old) == 1
    source = source.replace(old, '    if (f->mm) {\n        f->mm->users++;\n    }\n    return f->mm;\n')
path.write_text(source)
PY
        case $fault in
            ptmx) message='ptmx exclusive create returns EEXIST before dispatch' ;;
            terminator) message='runtime error: store to null pointer' ;;
            mem-hold) message='fail: an open of /proc/PID/mem holds the record and is no user of it' ;;
            mem-live) message='fail: a read once the process has ended returns 0 and reaches no memory' ;;
        esac
        run_phase "$test" "$fault" 1 "$message"
        cleanup
        run_phase "$test" "$fault-restored" 0 "$test-hosttest: result: pass:"
        cmp "$work/$test.saved" "$work/sys/fs/$test.c"
    done
done
