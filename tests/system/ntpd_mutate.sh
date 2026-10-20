#!/bin/sh
# ntpd_mutate.sh [--texts]: show that the gnu suite's ntpd row catches two
# faults, on x86-64.
#
# Usage: sh tests/system/ntpd_mutate.sh [--texts]
#
# The gnu suite runs its ntpd row alone four times: as the tree has it,
# where it must pass; with the row writing /etc/ntp.conf with its server
# line commented out, where init starts no ntpd and the clock is never set;
# with the SNTP server serving the host's own time in place of the time
# that began in 2030, where ntpd keeps the clock near the host's; and
# restored, where it must pass again. Both faults are in scripts the suite
# runs as they are, gnu_rows.sh, which it copies to the machine, and
# sntpserver.py, so nothing is built. --texts checks that each fault's text
# is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1

# mutate FILE OLD NEW: replace OLD, which must be once in FILE, with NEW;
# with RACH2_FAULT_TEXTS set, only check that OLD is once in FILE.
mutate()
{
    python3 - "$1" "$2" "$3" <<'PY'
import os
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit(f"ntpd-mutation: not once in {path}: {old}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

# fault NAME: put the fault NAME into its source.
fault()
{
    case $1 in
        no-server)
            mutate tests/system/gnu_rows.sh \
                "    printf 'server %s\\n' \"\${RACH2_NTP_SERVER:-}\" > /etc/ntp.conf" \
                "    printf '# server %s\\n' \"\${RACH2_NTP_SERVER:-}\" > /etc/ntp.conf"
            ;;
        host-time)
            mutate tests/system/sntpserver.py \
                '    return epoch + time.monotonic() - start' \
                '    return time.time()'
            ;;
    esac
}

if [ -n "$texts" ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in no-server host-time; do
        fault "$name"
    done
    echo 'ntpd-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ntpd-mutation "$repo/tests/system/ntpd_mutate.sh" "$@"
mkdir -p build
work=
child=
changed=
keep=

# Stop the running suite's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved sources and mark them unchanged.
restore()
{
    cp "$work/gnu_rows.sh.saved" tests/system/gnu_rows.sh
    cp "$work/sntpserver.py.saved" tests/system/sntpserver.py
    changed=
}

# Stop the suite and, when a fault is in a source, restore it. The work
# directory is removed too, unless the proof failed after printing its path,
# which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/ntpd-proof.XXXXXX")
cp tests/system/gnu_rows.sh "$work/gnu_rows.sh.saved"
cp tests/system/sntpserver.py "$work/sntpserver.py.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite's ntpd row. With EXPECTED
# `pass` it must pass, otherwise fail, and either way its output must have
# MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 RACH2_GNU_ROWS=ntpd build/tests/system/runutil group tests/run gnu \
        >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    for log in build/x86_64/images/gnu-test.disk.*; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*.disk.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "ntpd-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "ntpd-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "ntpd-mutation: result: $phase status $status"
}

echo "ntpd-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
run_phase fixed pass 'gnu-test: ntpd: OK'

changed=1
fault no-server
run_phase no-server fail "gnu-test: ntpd: FAIL: no answer within its bound"
restore

changed=1
fault host-time
run_phase host-time fail "gnu-test: ntpd: FAIL"
restore

run_phase restored pass 'gnu-test: ntpd: OK'
cmp "$work/gnu_rows.sh.saved" tests/system/gnu_rows.sh
cmp "$work/sntpserver.py.saved" tests/system/sntpserver.py
echo 'ntpd-mutation: result: pass: both faults failed the row; the restored scripts passed'
