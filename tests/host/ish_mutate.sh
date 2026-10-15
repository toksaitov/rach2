#!/bin/sh
# Prove ish_hosttest.c with private mutations of ish/ish.c and
# ish/ish_cstring_utilities.c.
#
# Usage: sh tests/host/ish_mutate.sh [FAULT ...] | --texts
# With no operand every fault is proved; an operand proves only the ones named.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR FAULT: put FAULT into its source in DIR, a copy of ish/;
# with RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

root = Path(sys.argv[1])
fault = sys.argv[2]
changes = {
    "interrupted": ("ish.c", [
        ("} while (waited == -1 && errno == EINTR);", "} while (0);"),
        ("} while (waited == -4);", "} while (0);"),
    ]),
    "failed": ("ish.c", [("if (waited != pid) {", "if (0) {")]),
    "partial": ("ish.c", [
        ("int i;\n        for (i = 0;", "int i;\n        length = 0;\n        for (i = 0;")
    ]),
    "eof": ("ish.c", [
        ("if (bytes_read == 0 && length != 0 && !discarding) {", "if (0) {")
    ]),
    "overflow": ("ish.c", [("                if (!discarding) {", "                if (1) {")]),
    "path": ("ish.c", [
        ("if (!cut && stat(candidate, stat_result) == 0) {",
         "if (stat(candidate, stat_result) == 0) {")
    ]),
    "join": ("ish_cstring_utilities.c", [
        ("    combined_path[i] = '\\0';\n    return result;",
         "    combined_path[i] = '\\0';\n    return 0;")
    ]),
}
if fault == "chunks":
    path = root / "ish.c"
    source = path.read_text()
    start = source.index("    char input[Max_Input_String_Length];")
    end = source.rindex("    return 0;")
    source = source[:start] + '''    char input[Max_Input_String_Length + 1];
    int bytes_read;
    while ((bytes_read = read(0, input, Max_Input_String_Length)) > 0) {
        input[bytes_read] = '\\0';
        ish_run_line(input, home, paths, envp);
    }

''' + source[end:]
elif fault in changes:
    name, edits = changes[fault]
    path = root / name
    source = path.read_text()
    for old, new in edits:
        if source.count(old) != 1:
            raise SystemExit("ish-mutation: source pattern is not unique: " + fault)
        source = source.replace(old, new)
else:
    raise SystemExit("ish-mutation: unknown fault: " + fault)
path.write_text(source)
PY
}

faults='interrupted failed chunks partial eof overflow path join'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault ish "$fault" >/dev/null || exit 1
    done
    echo 'ish-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ish-mutation "$repo/tests/host/ish_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ish-proof.XXXXXX")
mkdir -p "$work/tests/host" "$work/tests/musl"
cp -R ish "$work/ish"
cp tests/host/ish_hosttest.c "$work/tests/host/"
cp tests/musl/ishmusl.c "$work/tests/musl/"
cp "$work/ish/ish.c" "$work/ish.saved"
cp "$work/ish/ish_cstring_utilities.c" "$work/cstring.saved"
child=

# stop_child: end the running test and its session's process group, if there
# is one, and wait until none of its processes is left.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# cleanup: stop the running test and put the two saved sources back; the loop
# calls it after each fault, and it is the exit trap.
cleanup()
{
    stop_child
    cp "$work/ish.saved" "$work/ish/ish.c"
    cp "$work/cstring.saved" "$work/ish/ish_cstring_utilities.c"
    touch "$work/ish/ish.c" "$work/ish/ish_cstring_utilities.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile ish_hosttest.c over the copy twice,
# over the C library and with ISH_RAW_HOSTTEST, run each in a session of its
# own for at most 15 seconds, and exit unless each ends with STATUS and its
# log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    for mode in libc raw; do
        binary="$work/$phase.$mode"
        extra=
        [ "$mode" != raw ] || extra=-DISH_RAW_HOSTTEST
        if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
            -DRACH2_HOSTTEST -DISH_USE_STDLIB $extra -g -O1 -Wno-empty-body \
            -ffreestanding -fsanitize=address,undefined -fno-sanitize-recover=all \
            -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs \
            -I tests/host/compat "$work/tests/host/ish_hosttest.c" \
            "$work/ish/ish_shell_utilities.c" "$work/ish/ish_cstring_utilities.c" \
            -o "$binary" >"$binary.compile.log" 2>&1; then
            cat "$binary.compile.log" >&2
            exit 1
        fi
        cksum "$work/ish/ish.c" "$work/ish/ish_cstring_utilities.c" "$binary" >"$binary.cksum"
        python3 -c 'import os, signal, sys; os.setsid(); signal.alarm(15); os.execv(sys.argv[1], sys.argv[1:])' \
            "$binary" >"$binary.log" 2>&1 &
        child=$!
        status=0
        wait "$child" || status=$?
        stop_child
        cat "$binary.log"
        [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
        echo "ish-mutation: result: $phase $mode status $status"
    done
}

if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086 # the faults are separate words
    set -- $faults
fi
echo "ish-mutation: logs: $work"
run_phase fixed 0 'ish_hosttest: ok'
for fault do
    apply_fault "$work/ish" "$fault"
    case $fault in
        interrupted) message='ISHMUSL FAIL (interrupted wait)' ;;
        failed) message='ISHMUSL FAIL (failed wait)' ;;
        chunks) message='ISHMUSL FAIL (input lines)' ;;
        partial) message='ISHMUSL FAIL (short input reads)' ;;
        eof) message='ISHMUSL FAIL (final input line)' ;;
        overflow) message='ISHMUSL FAIL (long input line)' ;;
        path) message='ISHMUSL FAIL (long path entry)' ;;
        join) message='ISHMUSL FAIL (path joined past its limit)' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'ish_hosttest: ok'
    cmp "$work/ish.saved" "$work/ish/ish.c"
    cmp "$work/cstring.saved" "$work/ish/ish_cstring_utilities.c"
done
