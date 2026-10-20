#!/bin/sh
# etude_runtime_mutate.sh: prove etude_runtime_hosttest.c with faults put into
# private copies of etude's ledger, keeper, supervisor, repetition driver and
# store of blobs.
#
# Usage: sh tests/host/etude_runtime_mutate.sh [--texts]
#
# It builds and runs the host test on the unmodified sources, then on a copy
# of tools/etude with each fault below in it, requiring the test to fail with
# a failure in each row named for the fault, then on the restored copy. It
# changes no source in the working tree. --texts checks that each fault's
# text is once in its source, and builds nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# The faults: NAME|FILE|TEXT|REPLACEMENT|ROWS. FILE is under tools/etude;
# TEXT is a whole line of FILE, and REPLACEMENT the line put in its place.
faults() {
    cat <<'END'
over-admitted|ledger.c|        if (held(l, t, cap[k].name, 0, &sum) != 0 \|\| add(&sum, want) != 0 \|\| sum > cap[k].amount)|        if (held(l, t, cap[k].name, 0, &sum) != 0 \|\| add(&sum, want) != 0)|accounts
unprotected|ledger.c|    if (!fits(l, t, a, p == c ? NULL : p)) {|    if (!fits(l, t, a, NULL)) {|blocked
dead-request-kept|ledger.c|    if (strcmp(c->state, "reserved") == 0 \|\| is_request_state(c->state))|    if (strcmp(c->state, "reserved") == 0)|dead-request
output-unwatched|posix/keeper.c|        write_verdict(kp->args, confirmed && outputs_closed(kp, grace));|        write_verdict(kp->args, confirmed);|lost-worker
refusal-waits|posix/repeat.c|    if (errno == EAGAIN)|    if (errno == EAGAIN \|\| errno == ENOSPC)|lost-worker
not-abandoned|posix/repeat.c|            etude_supervised_cancel(s->cas);|            ;|full-disk
in-place|compare_report.c|    file = fopen(part, "wb");|    file = fopen(path, "wb");|full-disk
unwatched|supervise.c|    if (a->phase == PHASE_RUN && etude_watchdog_check(a->wd, now) != ETUDE_EXPIRY_NONE)|    if (0)|stuck
lost-released|supervise.c|    a->r.claim_kept = a->r.cleanup != ETUDE_CLEANUP_CONFIRMED;|    a->r.claim_kept = 0;|transport
report-unheard|supervise.c|        a->r.cleanup = a->reported_cleanup;|        a->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;|transport-again
END
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
            echo "etude-runtime-mutation: $(field "$f" 1): its text is $n times in $(field "$f" 2)" >&2
            exit 1
        fi
    done
    echo "etude-runtime-mutation: texts: every fault's text is once in its source"
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
        echo "etude-runtime-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-runtime-proof.XXXXXX")

# The test's sources, from its host-deps: lines, with tools/etude/ in the copy.
srcs=$(grep -oE 'host-deps:[^*]*' tests/host/etude_runtime_hosttest.c | sed 's/host-deps://' |
    tr ' ' '\n' | sed -e '/^$/d' -e "s#^tools/etude/#$work/etude/#" | tr '\n' ' ')

# run NAME [WARN]: build the host test against the copy in $work/etude, with
# warnings as errors unless WARN is given, and run it, leaving its output in
# $work/NAME.log and the fixtures a failure keeps under $work; the status is
# the test's, or 2 when it does not build.
run() {
    e=$work/etude
    strict=-Werror
    [ $# -lt 2 ] || strict=
    # shellcheck disable=SC2086
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -Wall -Wextra $strict -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I "$e/include" -I "$e" -I utils -I tests/host \
        -I tests/host/compat tests/host/etude_runtime_hosttest.c \
        $srcs \
        -o "$work/$1" >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        return 2
    }
    TMPDIR=$work perl -e 'alarm 240; exec @ARGV or die "etude-runtime-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    s=0
    wait "$child" || s=$?
    child=
    return "$s"
}

# fresh: copy tools/etude to $work/etude.
fresh() {
    rm -rf "$work/etude"
    cp -R tools/etude "$work/etude"
}

fresh
if ! run unmodified; then
    cat "$work/unmodified.log" >&2
    echo "etude-runtime-mutation: result: fail: the unmodified sources fail" >&2
    exit 1
fi
echo "etude-runtime-mutation: the unmodified sources pass"
bad=0
faults >"$work/faults"
while IFS= read -r f; do
    name=$(field "$f" 1)
    file=$work/etude/$(field "$f" 2)
    fresh
    awk -v t="$(field "$f" 3)" -v r="$(field "$f" 4)" \
        '$0 == t { print r; next } { print }' \
        "tools/etude/$(field "$f" 2)" >"$file"
    if cmp -s "$file" "tools/etude/$(field "$f" 2)"; then
        echo "etude-runtime-mutation: $name: the fault was not applied" >&2
        bad=1
        continue
    fi
    s=0
    run "$name" warn || s=$?
    if [ "$s" -ne 1 ]; then
        echo "etude-runtime-mutation: $name: the test gave status $s with the fault in, not 1" >&2
        bad=1
        continue
    fi
    missed=
    for row in $(field "$f" 5); do
        grep -q "^etude-runtime-test: fail: [^ ]*: $row: " "$work/$name.log" || missed="$missed $row"
    done
    if [ -n "$missed" ]; then
        echo "etude-runtime-mutation: $name: these rows did not fail:$missed" >&2
        cat "$work/$name.log" >&2
        bad=1
        continue
    fi
    echo "etude-runtime-mutation: $name: caught by $(field "$f" 5)"
done <"$work/faults"
fresh
if ! run restored; then
    cat "$work/restored.log" >&2
    echo "etude-runtime-mutation: result: fail: the restored sources fail" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "etude-runtime-mutation: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "etude-runtime-mutation: result: pass: every fault caught, and the restored sources pass"
