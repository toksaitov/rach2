#!/bin/sh
# Compile fixed, faulty and restored ISH sources in a private test tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ish-proof.XXXXXX")
mkdir -p "$work/tests/host" "$work/tests/musl"
cp -R ish "$work/ish"
cp tests/host/ish_hosttest.c "$work/tests/host/"
cp tests/musl/ishmusl.c "$work/tests/musl/"
cp "$work/ish/ish.c" "$work/ish.saved"
child=

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

cleanup()
{
    stop_child
    cp "$work/ish.saved" "$work/ish/ish.c"
    touch "$work/ish/ish.c"
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
        cksum "$work/ish/ish.c" "$binary" >"$binary.cksum"
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

echo "ish-mutation: logs: $work"
run_phase fixed 0 'ish_hosttest: ok'
for fault in interrupted failed chunks partial eof overflow; do
    python3 - "$work/ish/ish.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
changes = {
    "interrupted": [
        ("} while (waited == -1 && errno == EINTR);", "} while (0);"),
        ("} while (waited == -4);", "} while (0);"),
    ],
    "failed": [("if (waited != pid) {", "if (0) {")],
    "partial": [
        ("int i;\n        for (i = 0;", "int i;\n        length = 0;\n        for (i = 0;")
    ],
    "eof": [
        ("if (bytes_read == 0 && length != 0 && !discarding) {", "if (0) {")
    ],
    "overflow": [("                if (!discarding) {", "                if (1) {")],
}
if fault == "chunks":
    start = source.index("    char input[Max_Input_String_Length];")
    end = source.rindex("    return 0;")
    source = source[:start] + '''    char input[Max_Input_String_Length + 1];
    int bytes_read;
    while ((bytes_read = read(0, input, Max_Input_String_Length)) > 0) {
        input[bytes_read] = '\\0';
        ish_run_line(input, home, paths, envp);
    }

''' + source[end:]
else:
    for old, new in changes[fault]:
        if source.count(old) != 1:
            raise SystemExit("ish-mutation: source pattern is not unique")
        source = source.replace(old, new)
path.write_text(source)
PY
    case $fault in
        interrupted) message='ISHMUSL FAIL (interrupted wait)' ;;
        failed) message='ISHMUSL FAIL (failed wait)' ;;
        chunks) message='ISHMUSL FAIL (input lines)' ;;
        partial) message='ISHMUSL FAIL (short input reads)' ;;
        eof) message='ISHMUSL FAIL (final input line)' ;;
        overflow) message='ISHMUSL FAIL (long input line)' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'ish_hosttest: ok'
    cmp "$work/ish.saved" "$work/ish/ish.c"
done
