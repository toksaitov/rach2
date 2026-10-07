#!/bin/sh
# Compile Doom's fixed, faulty and restored control paths in a private tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" "$3" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

platform, system = map(Path, sys.argv[1:3])
changes = {
    "arguments": (platform, "    int doom_argc = (argc > 0 ? argc : 1) + 2;",
                  "    argc = 1;\n    int doom_argc = (argc > 0 ? argc : 1) + 2;"),
    "shutdown": (platform, "    DG_Shutdown();", ""),
    "exit": (system, "#endif\n    exit(0);\n}", "#endif\n}"),
    "report": (system, "    DG_zone_mib = *size / (1024 * 1024);", ""),
}
path, old, new = changes[sys.argv[3]]
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("doom-platform-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='arguments shutdown exit report'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault ports/doom/doomgeneric_rach2.c ports/doom/src/i_system.c "$fault" >/dev/null || exit 1
    done
    echo 'doom-platform-mutation: texts: every fault is once in its source'
    exit 0
fi

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/doom-platform-proof.XXXXXX")
mkdir -p "$work/ports" "$work/tests/host"
cp -R ports/doom "$work/ports/doom"
cp tests/host/doom_platform_hosttest.c "$work/tests/host/"
platform="$work/ports/doom/doomgeneric_rach2.c"
system="$work/ports/doom/src/i_system.c"
cp "$platform" "$work/platform.saved"
cp "$system" "$work/system.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/platform.saved" "$platform"
    cp "$work/system.saved" "$system"
    touch "$platform" "$system"
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
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding \
        -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs \
        -I tests/host/compat -I "$work/ports/doom/src" -DNORMALUNIX -DCMAP256 \
        "$work/tests/host/doom_platform_hosttest.c" -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$platform" "$system" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "doom-platform-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    grep -Fq 'doom-platform-hosttest: result:' "$binary.log" || exit 1
    echo "doom-platform-mutation: result: $phase status $status"
}

echo "doom-platform-mutation: logs: $work"
run_phase fixed 0 'doom-platform-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$platform" "$system" "$fault"
    case $fault in
        arguments) message='argument count' ;;
        shutdown) message='both devices closed once' ;;
        exit) message='quit must exit' ;;
        report) message='report names allocated zone' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'doom-platform-hosttest: result: pass:'
    cmp "$work/platform.saved" "$platform"
    cmp "$work/system.saved" "$system"
done
echo 'doom-platform-mutation: result: pass: four compiled faults failed; restored runs passed'
