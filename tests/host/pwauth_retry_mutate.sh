#!/bin/sh
# Prove counted credential retries and the report and terminal handshakes.
# Optional arguments select faults; without them every fault runs.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE FAULT: change one private source, or check its text only.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
fault = sys.argv[2]
text = path.read_text()
report = '        fprintf(stderr, "%s: cannot record the holder pid in %s\\n", prog, lock_path);'
unlink = '        failed = unlink(lock_path) != 0 && errno != ENOENT;'
restore = '        sigprocmask(SIG_SETMASK, &saved_mask, NULL);\n' + report
faults = {
    'no-wait': ('        sleep(1);', '        (void)0;'),
    'short-budget': ('if (attempt >= tries)', 'if (attempt >= tries - 1)'),
    'long-budget': ('if (attempt >= tries)', 'if (attempt > tries)'),
    'interval': ('        sleep(1);', '        sleep(2);'),
    'reentry': ('    if (cred_txn_mine()) {\n        fprintf',
                '    if (cred_txn_mine()) {\n        sleep(1);\n        fprintf'),
    'report-mask': (restore, report + '\n        sigprocmask(SIG_SETMASK, &saved_mask, NULL);'),
    'report-lock': (unlink, '        failed = 0;'),
    'teardown': ('    pwauth_stop_signals(&stops);\n    sigprocmask(SIG_BLOCK, &stops, &saved_mask);\n    if (Tty_echo_off)',
                 '    pwauth_stop_signals(&stops);\n    sigprocmask(SIG_BLOCK, NULL, &saved_mask);\n    if (Tty_echo_off)'),
}
old, new = faults[fault]
if text.count(old) != 1:
    raise SystemExit('pwauth-retry-mutation: source pattern is not unique: ' + fault)
text = text.replace(old, new)
if fault == 'report-lock':
    if text.count(report) != 1:
        raise SystemExit('pwauth-retry-mutation: report pattern is not unique')
    text = text.replace(report, report + '\n' + unlink)
if sys.argv[1] != 'utils/pwauth.c':
    path.write_text(text)
PY
}

faults='no-wait short-budget long-budget interval reentry report-mask report-lock teardown'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        apply_fault utils/pwauth.c "$fault"
    done
    echo 'pwauth-retry-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" pwauth-retry-mutation "$repo/tests/host/pwauth_retry_mutate.sh" "$@"
work=
# cleanup: restore the private source and remove scratch on every exit.
cleanup()
{
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pwauth-retry-proof.XXXXXX")
cp utils/pwauth.c "$work/pwauth.c"
make build/tests/system/deadline
"${CC:-cc}" -std=c99 -g -O1 -ffreestanding \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -c vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c -o "$work/crypt.o"

# run_phase NAME EXPECTED MESSAGE: compile and run the real library's copy.
# A fault must fail its own assertion. A deadline or sanitizer failure
# is no proof.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I utils -I tests/host/compat tests/host/pwauth_hosttest.c \
        "$work/pwauth.c" "$work/crypt.o" \
        -o "$binary"
    status=0
    build/tests/system/deadline 60 "$binary" >"$binary.log" 2>&1 || status=$?
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 100 ] || exit 1
        grep -Fq "$message" "$binary.log" || exit 1
        if grep -Eq 'AddressSanitizer|runtime error:' "$binary.log"; then
            exit 1
        fi
    fi
    echo "pwauth-retry-mutation: result: $phase status $status"
}

run_phase baseline pass ''
[ "$#" -eq 0 ] || faults="$*"
count=0
for fault in $faults; do
    apply_fault "$work/pwauth.c" "$fault"
    case $fault in
        no-wait|short-budget|long-budget) message='did not request exactly two retry intervals' ;;
        interval) message='requested an interval other than one second' ;;
        reentry) message='a second transaction in one process requested a retry' ;;
        report-mask) message='could not be killed by SIGTERM' ;;
        report-lock) message='has not yet removed its lock' ;;
        teardown) message='a continue during the teardown left the terminal with echo off' ;;
    esac
    run_phase "$fault-faulty" fail "$message"
    cp utils/pwauth.c "$work/pwauth.c"
    touch "$work/pwauth.c"
    run_phase "$fault-restored" pass ''
    count=$((count + 1))
done
echo "pwauth-retry-mutation: result: pass: $count faults failed; restored tests passed"
