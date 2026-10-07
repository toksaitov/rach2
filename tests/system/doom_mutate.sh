#!/bin/sh
# Prove the missing-WAD row of `tests/run doom` with compiled faults.
#
# Two faults are put into the game's sources in turn, each alone: `status`
# makes the platform's exit report 0 whatever status it is given, and `crash`
# takes out the engine's refusal of a missing IWAD, so the engine goes on
# with no IWAD and faults reading its absent name. The suite rebuilds the game
# and its disk from the sources as they stand, and the row must fail under
# each fault with the status or the missing error named. The unmodified suite
# passes first, and again after each source is restored, touched and rebuilt,
# with the game's bytes those of the first build.
#
# The game, the disk and the emulator are those of the architecture ARCH
# names.
#
# Usage: sh tests/system/doom_mutate.sh [--direct] [FAULT...] | --texts
#   --direct runs `make run-doom` instead of `tests/run doom`. FAULT is status
#   or crash; the default is both.
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

fault, name = sys.argv[1:]
path = Path(name)
text = path.read_text()
if fault == 'status':
    old, new = '    _exit(status);\n', '    _exit(0);\n'
else:
    old = '            I_Error("IWAD file \'%s\' not found!", iwadfile);\n'
    new = ''
assert text.count(old) == 1
path.write_text(text.replace(old, new))
PY
}

# select_fault FAULT: the source FAULT changes, in `changed`, and the words
# its failing row prints, in `expected`.
select_fault()
{
    case "$1" in
        status) changed=ports/doom/doomgeneric_rach2.c
            expected='without its WAD the game exited with status 0 and' ;;
        crash) changed=ports/doom/src/d_iwad.c
            expected='and 0 not-found line(s); the engine' ;;
    esac
}

if [ "${1:-}" = --texts ]; then
    for fault in status crash; do
        select_fault "$fault"
        RACH2_FAULT_TEXTS=1 apply_fault "$fault" "$changed" >/dev/null || exit 1
    done
    echo 'doom-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" doom-mutation "$repo/tests/system/doom_mutate.sh" "$@"

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
tree=build$ARCH_SUFFIX
runner=suite
if [ "${1-}" = --direct ]; then
    runner=direct
    shift
fi
if [ "$#" -eq 0 ]; then
    set -- status crash
fi
for fault do
    case "$fault" in
        status|crash) ;;
        *) echo "doom-mutation: unknown fault: $fault" >&2; exit 2 ;;
    esac
done
[ -f ports/doom/doom1.wad ] || {
    echo 'doom-mutation: ports/doom/doom1.wad is absent; the suite needs the WAD' >&2
    exit 1
}
mkdir -p build
work=$(mktemp -d "$repo/build/doom-proof.XXXXXX")
for source in ports/doom/doomgeneric_rach2.c ports/doom/src/d_iwad.c; do
    cp "$source" "$work/$(basename "$source").saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        "$work/runutil" killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

force_rebuild()
{
    rm -f "$tree/ports/doom/doomgeneric_rach2.o" "$tree/ports/doom/d_iwad.o" \
        "$tree/ports/doom/doom"
}

restore()
{
    if [ -n "$changed" ]; then
        cp "$work/$(basename "$changed").saved" "$changed"
        touch "$changed"
        changed=
    fi
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        force_rebuild
        make ports/doom "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# One phase: the game built from the sources as they stand, on its own so
# that a fault that does not compile stops the proof here, then the suite in
# a process group of its own. A passing phase must pass; a faulty one must
# fail with the words given.
run_phase()
{
    phase=$1
    expected=$2
    force_rebuild
    make ports/doom "ARCH=$RACH2_ARCH" >"$work/$phase.build.log" 2>&1 ||
        { cat "$work/$phase.build.log"; exit 1; }
    cksum "$tree/ports/doom/doom" >"$work/$phase.game"
    rm -f "$tree/images/doom-test.disk.doom.log"
    if [ "$runner" = direct ]; then
        "$work/runutil" group make run-doom "ARCH=$RACH2_ARCH" >"$work/$phase.log" 2>&1 &
    else
        ARCH=$RACH2_ARCH "$work/runutil" group tests/run doom >"$work/$phase.log" 2>&1 &
    fi
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    log=$tree/images/doom-test.disk.doom.log
    [ ! -f "$log" ] || cp "$log" "$work/$phase.console.log"
    cat "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || { echo "doom-mutation: $phase failed" >&2; exit 1; }
    else
        [ "$status" -ne 0 ] || { echo "doom-mutation: fault passed: $phase" >&2; exit 1; }
        grep -Fq "$expected" "$work/$phase.log" || {
            echo "doom-mutation: wrong failure: $phase" >&2; exit 1;
        }
    fi
    echo "doom-mutation: result: $phase status $status"
}

echo "doom-mutation: logs: $work"
run_phase fixed pass
for fault do
    select_fault "$fault"
    apply_fault "$fault" "$changed"
    touch "$changed"
    run_phase "$fault" "$expected"
    if cmp -s "$work/fixed.game" "$work/$fault.game"; then
        echo "doom-mutation: the $fault fault did not change the game" >&2
        exit 1
    fi
    restore
    run_phase "$fault-restored" pass
    cmp "$work/fixed.game" "$work/$fault-restored.game"
done
for source in ports/doom/doomgeneric_rach2.c ports/doom/src/d_iwad.c; do
    cmp "$source" "$work/$(basename "$source").saved"
done
echo "doom-mutation: pass: $# compiled faults failed the row; restored runs passed"
