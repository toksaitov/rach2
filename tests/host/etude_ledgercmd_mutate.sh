#!/bin/sh
# etude_ledgercmd_mutate.sh: prove etude_ledgercmd_hosttest.c with faults put
# into private copies of etude-ledger and the ledger it lists and drops.
#
# Usage: sh tests/host/etude_ledgercmd_mutate.sh [--texts]
#
# It builds and runs the host test on the unmodified sources, then on a copy
# of tools/etude with each fault below in it, requiring the test to fail with
# a failure in the row named for the fault, then on the restored copy. It
# changes no source in the working tree. --texts checks that each fault's
# text is once in its source, and builds nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# The faults: NAME|FILE|TEXT|REPLACEMENT|ROW. FILE is under tools/etude;
# TEXT is a whole line of FILE, and REPLACEMENT the line put in its place.
faults() {
    cat <<'EOF'
owned-dropped|ledger.c|    else if (judge(l, c, &why) == J_OWNED)|    else if (judge(l, c, &why) == J_OWNED && 0)|an owned claim is refused
owned-reported|posix/etude_ledger.c|    if (etude_ledger_drop(l, id) == 0) {|    if (etude_ledger_drop(l, id) == 0 \|\| errno == EBUSY) {|an owned claim is refused
live-dropped|posix/etude_ledger.c|    if (k.found && k.status == ETUDE_CLAIM_LIVE)|    if (0)|a live claim is refused
unnamed|ledger.c|            v->running = c->proc[i].id.pid;|            ;|a live claim is refused
missing|posix/etude_ledger.c|    if (errno == ENOENT) {|    if (errno == ENOENT && 0) {|a missing claim is refused
not-written|ledger.c|    e = end(l, &t, e, e == 0);|    e = end(l, &t, e, 0);|an uncertain claim is dropped
status-word|posix/etude_ledger.c|static const char *const STATUS[] = {"owned", "live", "uncertain"};|static const char *const STATUS[] = {"owned", "uncertain", "live"};|a live claim is listed with its reason
no-reason|posix/etude_ledger.c|        printf(": %s", c->why);|        ;|an uncertain claim is listed with its reason
request-as-claim|posix/etude_ledger.c|    return strcmp(c->state, "waiting") == 0 \|\| strcmp(c->state, "blocked") == 0;|    return strcmp(c->state, "waiting") == 0;|a request is listed
miscount|posix/etude_ledger.c|    count[c->status]++;|    count[0]++;|the listing counts each status
any-path|posix/etude_ledger.c|    if (!is_ledger(argv[2])) {|    if (0) {|a path that is not a ledger is refused
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
            echo "etude-ledgercmd-mutation: $(field "$f" 1): its text is $n times in $(field "$f" 2)" >&2
            exit 1
        fi
    done
    echo "etude-ledgercmd-mutation: texts: every fault's text is once in its source"
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
        echo "etude-ledgercmd-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-ledgercmd-proof.XXXXXX")

# run NAME [WARN]: build the host test against the copy in $work/tree, with
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
        "$work/tree/tests/host/etude_ledgercmd_hosttest.c" "$e/ledger.c" "$e/posix/process.c" \
        utils/sha256.c -o "$work/$1" >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        return 2
    }
    TMPDIR=$work perl -e 'alarm 60; exec @ARGV or die "etude-ledgercmd-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    s=0
    wait "$child" || s=$?
    child=
    return "$s"
}

# fresh: copy etude and the test into a private tree.
fresh() {
    rm -rf "$work/tree"
    mkdir -p "$work/tree/tools" "$work/tree/tests/host"
    cp -R tools/etude "$work/tree/tools/etude"
    cp tests/host/etude_ledgercmd_hosttest.c "$work/tree/tests/host/"
}

fresh
if ! run unmodified; then
    cat "$work/unmodified.log" >&2
    echo "etude-ledgercmd-mutation: result: fail: the unmodified sources fail" >&2
    exit 1
fi
echo "etude-ledgercmd-mutation: the unmodified sources pass"
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
        echo "etude-ledgercmd-mutation: $name: the fault was not applied" >&2
        bad=1
        continue
    fi
    s=0
    run "$name" warn || s=$?
    if [ "$s" -ne 1 ]; then
        echo "etude-ledgercmd-mutation: $name: the test gave status $s with the fault in, not 1" >&2
        bad=1
        continue
    fi
    row=$(field "$f" 5)
    if ! grep -Fq "etude-ledgercmd-test: fail: $row: " "$work/$name.log"; then
        echo "etude-ledgercmd-mutation: $name: its row did not fail: $row" >&2
        cat "$work/$name.log" >&2
        bad=1
        continue
    fi
    echo "etude-ledgercmd-mutation: $name: caught by $row"
done <"$work/faults"
fresh
if ! run restored; then
    cat "$work/restored.log" >&2
    echo "etude-ledgercmd-mutation: result: fail: the restored sources fail" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "etude-ledgercmd-mutation: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "etude-ledgercmd-mutation: result: pass: every fault caught, and the restored sources pass"
