#!/bin/sh
# etude_bridge_mutate.sh: prove etude_bridge_hosttest.c with faults put into
# private copies of the bridge and its external wrapper.
#
# Usage: sh tests/host/etude_bridge_mutate.sh [--texts]
#
# It builds and runs the host test on the unmodified sources, then on a copy
# of tools/etude with each fault below in it, requiring the test to fail with
# a failure in each row named for the fault, then on the restored copy. It
# changes no source in the working tree. The dead-worker row runs on Linux
# alone, so the proof runs there. --texts checks
# that each fault's text is once in its source, and builds nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# The faults: NAME|FILE|TEXT|REPLACEMENT|ROWS. FILE is under tools/etude;
# TEXT is a whole line of FILE, and REPLACEMENT the line put in its place.
faults() {
    cat <<'EOF'
shared-exclusive|posix/bridge.c|    short type = alone ? F_WRLCK : F_RDLCK;|    short type = F_RDLCK;|new exclusive excludes old
same-block|posix/bridge.c|                return slot * 1000;|                return 0;|old port avoided
release-live|posix/bridge.c|        if (etude_ledger_list(l, visit, &q) == 0 && !q.found)|        if (etude_ledger_list(l, visit, &q) == 0)|uncertain retains locks after death
native-overlap|posix/bridge.c|    return port >= 25000 && port <= 65535;|    return port >= 1024 && port <= 65535;|native avoids legacy range
no-offset|posix/etude_bridge.c|        setenv("RACH2_PORT_OFFSET", offset, 1);|        unsetenv("RACH2_PORT_OFFSET");|old checkout offset registration and status
no-wait|posix/etude_bridge.c|        if (seconds != 0 && e == EAGAIN && !stopped && posix_now_us() < until) {|        if (0) {|waits for admission
stop-as-refusal|posix/etude_bridge.c|    result(record, stopped ? "stopped" : "refused", stopped ? stopped : -1);|    result(record, "refused", -1);|stop recorded
incomplete-as-exit|posix/etude_bridge.c|    else if (rc < 0)|    else if (rc < -1)|incomplete recorded
status-as-refusal|posix/etude_bridge.c|        result(q.record, "exited", rc);|        result(q.record, "refused", -1);|command status kept apart
processors-judged|posix/etude_bridge.c|    while (etude_ledger_admit(l, id, account, res + 1, 1, cap + 1, 1, seconds != 0) != 0) {|    while (etude_ledger_admit(l, id, account, res, 2, cap, 2, seconds != 0) != 0) {|processors and disk not judged
disk-judged|posix/etude_bridge.c|    while (etude_ledger_admit(l, id, account, res + 1, 1, cap + 1, 1, seconds != 0) != 0) {|    while (etude_ledger_admit(l, id, account, res + 1, 2, cap + 1, 2, seconds != 0) != 0) {|processors and disk not judged
EOF
}

# field LINE N: the Nth field of a fault line, with `\|` read as `|`.
field() {
    printf '%s\n' "$1" | sed 's/\\|/\x01/g' | cut -d'|' -f"$2" | sed 's/\x01/|/g'
}

# count FILE TEXT: how many lines of FILE are TEXT.
count() {
    awk -v t="$2" '$0 == t { n++ } END { print n + 0 }' "$1"
}

if [ "${1:-}" = --texts ]; then
    faults | while IFS= read -r f; do
        n=$(count "tools/etude/$(field "$f" 2)" "$(field "$f" 3)")
        if [ "$n" -ne 1 ]; then
            echo "etude-bridge-mutation: $(field "$f" 1): its text is $n times in $(field "$f" 2)" >&2
            exit 1
        fi
    done
    echo "etude-bridge-mutation: texts: every fault's text is once in its source"
    exit 0
fi

work=
child=
stopped=

# Stop a running test before removing the private sources, which a failure
# keeps as its evidence.
cleanup() {
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "$stopped" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-bridge-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-bridge-proof.XXXXXX")

# run NAME [WARN]: build the host test against the copy in $work/etude, with
# warnings as errors unless WARN is given, and run it, leaving its output in
# $work/NAME.log and the fixtures a failure keeps under $work; the status is
# the test's, or 2 when it does not build.
run() {
    e=$work/tree/tools/etude
    strict=-Werror
    [ $# -lt 2 ] || strict=
    # shellcheck disable=SC2086
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -Wall -Wextra $strict -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I "$e/include" -I utils -I tests/host -I tests/host/compat \
        "$work/tree/tests/host/etude_bridge_hosttest.c" "$e/posix/bridge.c" "$e/ledger.c" "$e/capture.c" utils/sha256.c \
        "$e/posix/exec.c" "$e/posix/keeper.c" "$e/posix/process.c" \
        -o "$work/$1" >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        return 2
    }
    TMPDIR=$work perl -e 'alarm 60; exec @ARGV or die "etude-bridge-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    s=0
    wait "$child" || s=$?
    child=
    return "$s"
}

# fresh: copy the bridge and its test into a private tree.
fresh() {
    rm -rf "$work/tree"
    mkdir -p "$work/tree/tools" "$work/tree/tests/host"
    cp -R tools/etude "$work/tree/tools/etude"
    cp tests/host/etude_bridge_hosttest.c "$work/tree/tests/host/"
}

fresh
if ! run unmodified; then
    cat "$work/unmodified.log" >&2
    echo "etude-bridge-mutation: result: fail: the unmodified sources fail" >&2
    exit 1
fi
echo "etude-bridge-mutation: the unmodified sources pass"
bad=0
faults >"$work/faults"
while IFS= read -r f; do
    name=$(field "$f" 1)
    file=$work/tree/tools/etude/$(field "$f" 2)
    fresh
    awk -v t="$(field "$f" 3)" -v r="$(field "$f" 4)" \
        '$0 == t { print r; next } { print }' \
        "tools/etude/$(field "$f" 2)" >"$file"
    if cmp -s "$file" "tools/etude/$(field "$f" 2)"; then
        echo "etude-bridge-mutation: $name: the fault was not applied" >&2
        bad=1
        continue
    fi
    s=0
    run "$name" warn || s=$?
    if [ "$s" -ne 1 ]; then
        echo "etude-bridge-mutation: $name: the test gave status $s with the fault in, not 1" >&2
        bad=1
        continue
    fi
    row=$(field "$f" 5)
    if ! grep -Fq "etude-bridge-test: fail: $row: " "$work/$name.log"; then
        echo "etude-bridge-mutation: $name: its row did not fail: $row" >&2
        cat "$work/$name.log" >&2
        bad=1
        continue
    fi
    echo "etude-bridge-mutation: $name: caught by $(field "$f" 5)"
done <"$work/faults"
fresh
if ! run restored; then
    cat "$work/restored.log" >&2
    echo "etude-bridge-mutation: result: fail: the restored sources fail" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "etude-bridge-mutation: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "etude-bridge-mutation: result: pass: every fault caught, and the restored sources pass"
