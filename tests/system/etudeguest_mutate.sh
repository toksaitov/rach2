#!/bin/sh
# etudeguest_mutate.sh [--texts]: show that the etudeguest suite catches three
# faults compiled into etude-guest, on the kernel, image and emulator of the
# architecture ARCH names.
#
# Usage: sh tests/system/etudeguest_mutate.sh [--texts]
#
# etude-guest is built from a private copy of its source, as it is, with a
# repeated command run again, with a cancellation reported as the signal that
# ended the program, and with exec running the program as a child; each build
# is staged into a private copy of the test image, from the test manifest
# with only etude-guest's line changed, and the suite's driver runs against
# it. The unchanged copy must pass, each fault must fail the step it breaks,
# and the copy restored must pass and match the first build byte for byte.
# Nothing in the tree is changed. --texts checks that each fault is once in
# its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE NAME: put the fault NAME into the copy FILE; with
# RACH2_FAULT_TEXTS set, only check that its text is once in FILE.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'repeat': ('    if (read_file(path, buf, sizeof(buf)) < 0) {\n        char line0',
               '    if (1) {\n        char line0'),
    'stopped': ('    if (a->stopped)\n        snprintf(line[0]',
                '    if (0)\n        snprintf(line[0]'),
    'exec': ('    execve(exec_next.prog, exec_next.argv, exec_next.envp);\n    _exit(127);',
             '    if (fork() == 0)\n        execve(exec_next.prog, exec_next.argv, exec_next.envp);\n'
             '    _exit(0);'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etudeguest-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in repeat stopped exec; do
        apply_fault tools/etude/guest/guest.c "$name"
    done
    echo 'etudeguest-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etudeguest-mutation "$repo/tests/system/etudeguest_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
tree=build$ARCH_SUFFIX
work=
logged=
child=

# Stop the driver and its machine, and wait until they are gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}

# Stop the boot, then remove the scratch directory. A failure after its path
# was printed keeps it as evidence; a signal does not.
cleanup()
{
    status=$?
    [ -n "$work" ] || return 0
    stop_child
    case $status in
        0|129|130|143) ;;
        *) [ -z "$logged" ] || return 0 ;;
    esac
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rach2-etudeguest-proof.XXXXXX")
echo "etudeguest-mutation: logs: $work"
logged=1
make -s rach2 tools/etude/guest test-image utils build/tests/system/etudeserial \
    "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
mkdir "$work/src"
cp tools/etude/guest/guest.c "$work/src/guest.c"
sed "s|^file 0755 /etude-guest build/tools/etude/guest/etude-guest\$|file 0755 /etude-guest $work/bin/etude-guest|" \
    krffs/manifests/test.manifest >"$work/test.manifest"
grep -q "^file 0755 /etude-guest $work/bin/etude-guest\$" "$work/test.manifest"

# run_phase PHASE EXPECTED [WORDS]: build etude-guest from the private copy,
# stage the private image, and run the driver. With EXPECTED `pass` every
# step must pass; otherwise the driver must fail and print WORDS.
run_phase()
{
    phase=$1
    expected=$2
    rm -rf "${work:?}/bin"
    make -s -C tools/etude/guest "ARCH=$RACH2_ARCH" "BUILD=$work/bin" \
        "SRC=$work/src/guest.c wire.c ../history_json.c ../../../utils/sha256.c" \
        >"$work/$phase.build.log" 2>&1 || { cat "$work/$phase.build.log"; exit 1; }
    cp "$work/bin/etude-guest" "$work/$phase.etude-guest"
    sh krffs/mkimage.sh "$work/test.manifest" "$work/test.krffs" 13 5120 \
        >>"$work/$phase.build.log" 2>&1 || { cat "$work/$phase.build.log"; exit 1; }
    status=0
    build/tests/system/etudeserial --kernel "$tree/rach2/kernel" --initrd "$work/test.krffs" \
        --hello "$tree/tests/hello" --ish "$tree/ish/ish" --yes "$tree/utils/yes" \
        --log "$work/$phase.serial.log" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "etudeguest-mutation: the suite passed with its fault: $phase" >&2; exit 1;
        }
        grep -F "etudeserial: fail: $3" "$work/$phase.log" >/dev/null || exit 1
    fi
    echo "etudeguest-mutation: result: $phase status $status"
}

run_phase fixed pass
apply_fault "$work/src/guest.c" repeat
run_phase repeat fail 'a repeated run'
cp tools/etude/guest/guest.c "$work/src/guest.c"
apply_fault "$work/src/guest.c" stopped
run_phase stopped fail 'a cancelled program is stopped'
cp tools/etude/guest/guest.c "$work/src/guest.c"
apply_fault "$work/src/guest.c" exec
run_phase exec fail "the kernel records the designated init's exit"
cp tools/etude/guest/guest.c "$work/src/guest.c"
run_phase restored pass
cmp "$work/fixed.etude-guest" "$work/restored.etude-guest"
echo 'etudeguest-mutation: result: pass: each fault failed its step; the restored build passed and matches the first'
