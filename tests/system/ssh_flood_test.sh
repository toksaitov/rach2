#!/bin/sh
# Exercise the SSH suite's actual flood functions without opening sockets.
set -eu
case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'ssh-flood: usage: ssh_flood_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" ssh-flood "$repo/tests/system/ssh_flood_test.sh" "$@"
fi
if [ "$mutate" -eq 2 ]; then
    sed '/exec sleep 300/,/FLOOD_PIDS=/s/FLOOD_PIDS=.*$/ :/' "$repo/tests/system/suites/ssh.sh" |
        cmp -s - "$repo/tests/system/suites/ssh.sh" && {
        echo 'ssh-flood: the flood functions no longer record their sleep' >&2; exit 1;
    }
    echo 'ssh-flood: texts: every fault is once in its source'
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ssh-flood.XXXXXX")
driver=0
cp "$repo/tests/system/suites/ssh.sh" "$work/saved.sh"
cp "$work/saved.sh" "$work/source.sh"

# Stop all proof children before restoring its private copy of the source.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$driver" -ne 0 ]; then
        kill -TERM "$driver" 2>/dev/null || :
        wait "$driver" 2>/dev/null || :
    fi
    for file in "$work"/*/pids/*; do
        [ -f "$file" ] || continue
        read -r pid < "$file"
        kill -KILL "$pid" 2>/dev/null || :
    done
    cp "$work/saved.sh" "$work/source.sh"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
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

# Extract cleanup and its traps as well as the two flood functions. Fake
# sleep and nc retain their PIDs through exec; their records only test cleanup.
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
if [ "$mutate" -eq 1 ]; then
    sed '/exec sleep 300/,/FLOOD_PIDS=/s/FLOOD_PIDS=.*$/ :/' "$work/saved.sh" > "$work/source.sh"
    cmp -s "$work/saved.sh" "$work/source.sh" && exit 1
    rc=0
    check_case missing-sleep success || rc=$?
    [ "$rc" -eq 1 ] || { echo 'ssh-flood: missing sleep ownership was not detected' >&2; exit 1; }
    echo 'ssh-flood: result: missing sleep ownership detected'
    cp "$work/saved.sh" "$work/source.sh"
    touch "$work/source.sh"
    check_case restored success
    check_case restored signal
fi
