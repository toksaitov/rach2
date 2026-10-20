#!/bin/sh
# etude_budget_mutate.sh: prove etude_budget_hosttest.c with faults put into
# private copies of etude's budget, its digests and history_budget.
#
# Usage: sh tests/host/etude_budget_mutate.sh [--texts]
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
    cat <<'EOF'
growth-unchecked|budget.c|        if (l->goal_us > l->ceiling_us \|\| l->growth_us > l->ceiling_us - l->goal_us \|\||        if (l->goal_us > l->ceiling_us \|\||policy
arch-digest|digest.c|    give(&d, &arch, "architecture", architecture);|    (void)arch;|digests
placement-digest|digest.c|    give(&d, &place, "placement", placement);|    (void)place;|digests
services-digest|digest.c|    take(&d, c, "services");|    (void)c;|services
backfill|budget.c|        if (want > left[k] \|\| (want > 0 && kept > left[k] - want)) {|        if (want > left[k]) {|schedule
large-kept|budget.c|                jobs[i].state = ETUDE_JOB_REFUSED;|                jobs[i].state = ETUDE_JOB_PENDING;|schedule deadline
item-key|budget.c|#define ITEM_FIELDS 9|#define ITEM_FIELDS 4|unknown-cost
item-unknown|budget.c|        if (cost.samples == 0) {|        if (0) {|unknown-cost
one-repetition|budget.c|    for (rep = 0; cost.samples > 0 && rep < reps; rep++) {|    for (rep = 0; cost.samples > 0 && rep < 1; rep++) {|forecast
node-site|budget.c|    return on_site(p, x->env);|    return 1;|unknown-cost
node-class|budget.c|    return same(purpose, class_names[cls]) \|\||    return 1 \|\||unknown-node
node-unknown|budget.c|        if (count == 0) {|        if (0) {|unknown-node
overrun-ignored|budget.c|    if (charged > limit) {|    if (0) {|deadline ceiling
deadline-known|budget.c|    report_charge(l, f->prep_us + f->cases_us, l->ceiling_us - l->reserve_us,|    report_charge(l, f->report.unknown ? 0 : f->prep_us + f->cases_us, l->ceiling_us - l->reserve_us,|deadline
policy-limits|budget.c|        number_of(b, "ceiling_us") != (int64_t)l->ceiling_us \|\||        0 \|\||policy-record
site-ignored|budget.c|    if (!on_site(v->p, env)) {|    if (0) {|site
exemption-attempts|budget.c|            if (!listed(eh_json_get(ex, "attempt_ids"), id->string)) {|            if (0) {|exempt
exemption-lost|budget.c|    return j->report.failed == before;|    return 0;|exempt
reuse-allowed|budget.c|    if (strcmp(a->run, v->run_id) != 0) {|    if (0) {|reuse
summary-ignored|budget.c|        if (summary == NULL) {|        if (0) {|omitted
node-unprepared|budget.c|        if (!prepared(v, target)) {|        if (0) {|hidden-node
foreign-step|budget.c|        } else if (strcmp(r->run, v->run_id) != 0) {|        } else if (0) {|hidden-foreign
unlisted-step|budget.c|        if (strcmp(r->run, v->run_id) == 0 && shared_step(r) && !listed(prep, r->id)) {|        if (0) {|hidden-unlisted
cleanup-ignored|budget.c|    if (same(cmp_text(result, "cleanup"), "unconfirmed")) {|    if (0) {|unobserved
other-clock|budget.c|        if (shared_step(r) \|\| strcmp(r->kind, "attempt") == 0) {|        if (0) {|hidden-clock
early-hidden|budget.c|    if ((uint64_t)start < v->lo) {|    if (0) {|hidden-early
one-time-again|budget.c|    } else if (before != NULL) {|    } else if (0) {|one-time-again
one-time-overlap|budget.c|    } else if ((uint64_t)end > v->work_start) {|    } else if (0) {|one-time-overlap
error-bound|budget.c|        if (j->report.failed == 0 && v->error_us > l->ceiling_us - j->charged_us) {|        if (0) {|ceiling
record-unknown|budget.c|    int ok = same(verdict, "unknown") \|\| same(verdict, verdict_names[j->report.verdict]);|    int ok = same(verdict, verdict_names[j->report.verdict]);|unjudged
record-charge|budget.c|        ok = ok && charged == (int64_t)j->charged_us && once == (int64_t)j->one_time_us;|        ok = ok && 1;|hidden-early one-time-again
notes-ignored|budget.c|    for (i = 0; i < h->note_count; i++) {|    for (i = 0; i < 0; i++) {|unobserved
status-agrees|budget_run.c|    status = print_report(&j.report, j.agrees);|    status = print_report(&j.report, 1);|program
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
            echo "etude-budget-mutation: $(field "$f" 1): its text is $n times in $(field "$f" 2)" >&2
            exit 1
        fi
    done
    echo "etude-budget-mutation: texts: every fault's text is once in its source"
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
        echo "etude-budget-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-budget-proof.XXXXXX")

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
        tests/host/etude_budget_hosttest.c "$e/budget.c" "$e/budget_run.c" "$e/digest.c" \
        "$e/cost.c" "$e/sched.c" "$e/ledger.c" "$e/posix/process.c" "$e/compare.c" \
        "$e/compare_report.c" "$e/history.c" "$e/history_json.c" "$e/history_collect.c" \
        "$e/json.c" "$e/schema.c" utils/sha256.c \
        -o "$work/$1" >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        return 2
    }
    TMPDIR=$work perl -e 'alarm 120; exec @ARGV or die "etude-budget-mutation: exec: $!\n"' \
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
    echo "etude-budget-mutation: result: fail: the unmodified sources fail" >&2
    exit 1
fi
echo "etude-budget-mutation: the unmodified sources pass"
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
        echo "etude-budget-mutation: $name: the fault was not applied" >&2
        bad=1
        continue
    fi
    s=0
    run "$name" warn || s=$?
    if [ "$s" -ne 1 ]; then
        echo "etude-budget-mutation: $name: the test gave status $s with the fault in, not 1" >&2
        bad=1
        continue
    fi
    missed=
    for row in $(field "$f" 5); do
        grep -q "^etude-budget-test: fail: [^ ]*: $row: " "$work/$name.log" || missed="$missed $row"
    done
    if [ -n "$missed" ]; then
        echo "etude-budget-mutation: $name: these rows did not fail:$missed" >&2
        cat "$work/$name.log" >&2
        bad=1
        continue
    fi
    echo "etude-budget-mutation: $name: caught by $(field "$f" 5)"
done <"$work/faults"
fresh
if ! run restored; then
    cat "$work/restored.log" >&2
    echo "etude-budget-mutation: result: fail: the restored sources fail" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "etude-budget-mutation: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "etude-budget-mutation: result: pass: every fault caught, and the restored sources pass"
