#!/bin/sh
# test_status_check.sh: run tests/system/test_status_test.py, the test of the
# status reader tools/flow/test_status.py against live and stale claims of a
# tree, with etude-ports compiled from the tree into a scratch directory. The
# reader reads /proc/locks, so on a host that is not Linux the test runs in
# etude's tooling machine (tests/system/tooling.sh), as an ordinary user.
#
# Usage: sh tests/system/test_status_check.sh
#
# It ends with `test-status-check: result: pass` and status 0 when the test
# passed; status 1 when it failed; and status 2 when it did not run: the
# helper did not compile, the test skipped the host, or the host is not Linux
# and has no made tooling machine.
set -u
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo" || exit 2
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-test-status.XXXXXX") || exit 2

# The sources of etude-ports, and the headers they include.
programs='tools/etude/etude_ports.c tools/etude/portlock.c tools/etude/win/winfmt.c'
headers='tools/etude/win/winfmt.h tools/etude/include/etude/portlock.h'

# judge OUT STATUS: report the test's output OUT and its status.
judge() {
    cat "$1"
    if [ "$2" -ne 0 ]; then
        echo "test-status-check: result: fail: test_status_test.py exited with status $2"
        exit 1
    fi
    if grep -q 'result: skipped' "$1"; then
        echo 'test-status-check: result: not run: the test skipped this host'
        exit 2
    fi
    echo 'test-status-check: result: pass'
    exit 0
}

if [ "$(uname -s)" = Linux ]; then
    # shellcheck disable=SC2086 # the sources are a list
    cc -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -I tools/etude/include -I tools/etude \
        -o "$work/etude-ports" $programs || {
        echo 'test-status-check: result: not run: etude-ports did not compile'
        exit 2
    }
    status=0
    python3 tests/system/test_status_test.py "$work/etude-ports" > "$work/out" 2>&1 || status=$?
    judge "$work/out" "$status"
fi

REPO=$repo
# shellcheck source=tests/system/tooling.sh
. "$repo/tests/system/tooling.sh"
tooling_made || {
    echo 'test-status-check: result: not run: this host is not Linux and has no made tooling machine'
    exit 2
}
# shellcheck disable=SC2086 # the sources are a list
COPYFILE_DISABLE=1 tar -c --format ustar -f "$work/status.tar" tests/system/test_status_test.py \
    tools/flow/test_status.py $programs $headers || {
    echo "test-status-check: result: not run: the test's files could not be packed"
    exit 2
}
cat > "$work/job.sh" <<'JOB'
set -u
mkdir tree && tar -x -f status.tar -C tree && chmod -R a+rX tree &&
    useradd --create-home --user-group status || exit 3
cd tree && exec setpriv --reuid=status --regid=status --init-groups env -i PATH="$PATH" \
    HOME=/home/status sh -c 'cc -std=c99 -D_DEFAULT_SOURCE -O2 -I tools/etude/include -I tools/etude \
        -o "$HOME/etude-ports" tools/etude/etude_ports.c tools/etude/portlock.c tools/etude/win/winfmt.c &&
        python3 tests/system/test_status_test.py "$HOME/etude-ports"'
JOB
status=0
tooling run -i "$work/status.tar" "$work/job.sh" > "$work/out" 2>&1 || status=$?
case $status in
    124|125|126)
        cat "$work/out"
        echo "test-status-check: result: not run: the tooling machine's job did not run to its end, status $status"
        exit 2 ;;
esac
judge "$work/out" "$status"
