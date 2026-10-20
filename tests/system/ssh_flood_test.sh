#!/bin/sh
# ssh_flood_test.sh: check that the SSH suite's flood functions leave none of
# the children they start, with fake sleep and nc programs and no sockets.
#
# The functions and the cleanup traps are copied out of
# tests/system/suites/ssh.sh and run by a driver that starts the flood and
# then either stops it or is sent SIGTERM. Either way none of the 80
# children may be left, nor the flood's directory. SSH_SUITE names the suite
# to copy them from, by default the tree's; tests/proofs/ssh-flood.json runs
# the test against a copy whose flood_start does not record the process ID of
# a holder's sleep.
set -eu
[ "$#" -eq 0 ] || { echo 'ssh-flood: usage: ssh_flood_test.sh' >&2; exit 2; }
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ssh-flood "$repo/tests/system/ssh_flood_test.sh" "$@"
work=
driver=0
keep=

# Stop the driver and kill every child whose process ID is recorded. Then
# remove the work directory, unless the test failed after printing its path,
# which sets `keep`; a signal clears `keep`.
# shellcheck disable=SC2329
cleanup()
{
    exit_status=$?
    if [ "$driver" -ne 0 ]; then
        kill -TERM "$driver" 2>/dev/null || :
        wait "$driver" 2>/dev/null || :
    fi
    [ -n "$work" ] || return 0
    for file in "$work"/*/pids/*; do
        [ -f "$file" ] || continue
        read -r pid < "$file"
        kill -KILL "$pid" 2>/dev/null || :
    done
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
trap 'keep=; exit 129' HUP
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ssh-flood.XXXXXX")
cp "${SSH_SUITE:-$repo/tests/system/suites/ssh.sh}" "$work/source.sh"
mkdir "$work/bin"
cat > "$work/bin/sleep" <<'PEER'
#!/bin/sh
echo $$ > "$FLOOD_TEST_DIR/pids/sleep.$$"
exec /bin/sleep 300
PEER
cat > "$work/bin/nc" <<'PEER'
#!/bin/sh
echo $$ > "$FLOOD_TEST_DIR/pids/nc.$$"
exec /bin/sleep 300
PEER
chmod +x "$work/bin/sleep" "$work/bin/nc"
echo "ssh-flood: artifacts: $work"
keep=1

# check_case PHASE MODE: copy flood_start, flood_stop, cleanup, cleanup_boot
# and the traps that call cleanup out of the copy of the suite, and run a
# driver that starts the flood and waits until its 80 children have recorded
# their process IDs. With MODE `success` the driver calls flood_stop; with
# `signal` it is sent SIGTERM. Return 1 when a child or the flood's directory
# is left, and 125 when the case could not be run. The fake sleep and nc keep
# their process IDs through exec, so each record names the process to look
# for.
check_case()
{
    phase=$1
    mode=$2
    dir=$work/$phase.$mode
    mkdir -p "$dir/pids" || return 125
    awk '
        /^(flood_start|flood_stop|cleanup|cleanup_boot)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
        /^trap .*cleanup/ {print}
    ' "$work/source.sh" > "$dir/functions" || return 125
    cat > "$dir/driver" <<'DRIVER' || return 125
#!/bin/sh
set -eu
QPID="" FPID="" FLOOD_PIDS=""
WORK="$FLOOD_TEST_DIR/disk"
LOG="$WORK.log" FEED="$WORK.feed" KNOWN="$WORK.known"
FLOOD_DIR="$WORK.flood" PORT=1
. "$FLOOD_TEST_DIR/functions"
flood_start
i=0
while [ "$(ls "$FLOOD_TEST_DIR/pids" | wc -l)" -lt 80 ]; do
    i=$((i + 1))
    [ "$i" -lt 500 ] || exit 125
    /bin/sleep 0.02
done
echo ready > "$FLOOD_TEST_DIR/ready"
if [ "$1" = signal ]; then
    while :; do /bin/sleep 0.1; done
fi
flood_stop
DRIVER
    FLOOD_TEST_DIR=$dir PATH="$work/bin:$PATH" sh "$dir/driver" "$mode" > "$dir/output.log" 2>&1 &
    driver=$!
    if [ "$mode" = signal ]; then
        i=0
        while [ ! -s "$dir/ready" ] && kill -0 "$driver" 2>/dev/null && [ "$i" -lt 500 ]; do
            /bin/sleep 0.02; i=$((i + 1))
        done
        [ -s "$dir/ready" ] || return 125
        kill -TERM "$driver"
    fi
    status=0
    wait "$driver" || status=$?
    driver=0
    case "$mode:$status" in success:0|signal:143) : ;; *) return 125 ;; esac
    result=0
    for file in "$dir"/pids/*; do
        read -r pid < "$file"
        if kill -0 "$pid" 2>/dev/null; then
            echo "ssh-flood: $phase $mode left pid $pid" >&2
            result=1
            kill -KILL "$pid" 2>/dev/null || :
            i=0
            while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do
                /bin/sleep 0.02; i=$((i + 1))
            done
            kill -0 "$pid" 2>/dev/null && return 125
        fi
        rm "$file"
    done
    [ ! -d "$dir/disk.flood" ] || result=1
    [ "$result" -eq 0 ] || return 1
    echo "ssh-flood: result: $phase $mode reaped all 80 children"
}

check_case fixed success
check_case fixed signal
