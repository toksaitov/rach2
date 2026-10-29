#!/bin/sh
#
# tools/ci/report.sh: write down what a full run of the release tests failed.
#
#   tools/ci/report.sh SHA DIR FILE
#
# DIR has one directory for each architecture, `full-ARCH`, written by
# tools/ci/run.sh at the commit SHA. An architecture passed when its
# directory has `done` and an empty `failed`, or `done` and `skipped`, where
# it had nothing to run. FILE gets one line for each failure, `ARCH:ITEM`,
# ITEM being a suite, `build`, `runner` or the name of the step that failed
# without naming a suite, as run.sh writes them, or `unfinished` for an
# architecture whose directory has no `done`. full.yml uploads FILE as the
# artifact `failures`, which tools/flow/release.sh reads.
#
# Native result directories have `native` and `suites`; their shared parent
# has one history and store. The strict C verdict validates that complete
# history and its charged budget before projecting this architecture's result.
# Missing native history cannot use the old result-lines fallback. The
# reporting host supplies its installed reader, ETUDE_REPORT_READER or ETUDE,
# and its record, ETUDE_REPORT_READER_RECORD. An architecture whose verdict
# fails, or cannot be read, while `failed` is empty, failed with `runner`.
#
# Each architecture's result, with the first lines of `err` that name a
# failure and the command that fetches the logs, also goes to the job's
# summary, GITHUB_STEP_SUMMARY. The exit status is 0 when both architectures
# passed and 1 otherwise.
#
set -u

sha=${1-}
dir=${2-}
file=${3-}
[ -n "$sha" ] && [ -n "$dir" ] && [ -n "$file" ] ||
    { echo "usage: tools/ci/report.sh SHA DIR FILE" >&2; exit 2; }
short=$(echo "$sha" | cut -c 1-8)
# The lines of tests/run's results that name an item or a suite that did not
# pass, a change that voided the run, or the run's result when it failed.
failing='^run: (result: (failed|incomplete|refused|interrupted)|voided: )|: (FAIL|INCOMPLETE|CANCELLED) in |: (failure|incomplete|cancelled): '
native_expected=
grep -qx 'results=native' tools/ci/run.sh 2> /dev/null && native_expected=1
: > "$file" || exit 2

# Append each LINE to the job's summary.
summarize() { # summarize LINE...
    [ -z "${GITHUB_STEP_SUMMARY:-}" ] || printf '%s\n' "$@" >> "$GITHUB_STEP_SUMMARY"
}

# Set `history` to what the native verdict of the run in RESULT says, and
# `hstatus` to its status: 0 when it passed, 1 when it failed, and 2 when the
# history could not be read, `history` then saying why. A RESULT that is not
# native gives 0 and nothing. A backquote becomes a quote, so that the text
# stays in its fence.
read_history() { # read_history RESULT
    history=
    hstatus=0
    [ -n "$native_expected" ] || [ -f "$1/native" ] || return 0
    common=$1/..
    if ! grep -qx 'etude-native-1' "$1/native" 2> /dev/null || [ ! -s "$1/suites" ] ||
        [ ! -d "$common/history" ] || [ ! -d "$common/store" ]; then
        history='ci: report: native collection history or selection is missing'
        hstatus=2
        return 0
    fi
    reader=${ETUDE_REPORT_READER:-${ETUDE:-build/tools/etude/etude}}
    native_site=${ETUDE_SITE:-}
    if [ -z "$native_site" ]; then
        case $(uname -s) in
            MINGW*|MSYS*|UCRT*|CLANG*|CYGWIN*) native_site=C:/ProgramData/rach2/etude-site ;;
            *) native_site=/etc/rach2/etude-site ;;
        esac
    fi
    native_program=${ETUDE:-$(sed -n 's/^etude //p' "$native_site")}
    if [ -z "${ETUDE_REPORT_READER_RECORD:-}" ] ||
        ! reader=$("$native_program" prepare reader "$reader" "$ETUDE_REPORT_READER_RECORD") ||
        ! "$reader" check record -d "$ETUDE_REPORT_READER_RECORD" >&2; then
        history='ci: report: installed native reader did not verify'
        hstatus=2
        return 0
    fi
    native_suites=$(cat "$1/suites")
    if [ -f "$common/routine-completion.path" ]; then
        set -- --routine-completion "$common/routine.completion.json" \
            --routine-proofs "$common/routine-proofs"
    else
        set --
    fi
    history=$("$reader" check verdict "$@" --project-architecture --require-budget \
        -b "$common/store" -c "$sha" "$arch" full "$native_suites" \
        "$common/history" 2>&1) || hstatus=$?
    [ "$hstatus" -le 1 ] || hstatus=2
    history=$(printf '%s\n' "$history" | tr '\140' "'")
}

rc=0
summarize "## Release tests of $short" ""
[ -z "${SUITES:-}" ] || summarize "This run tested only these suites: $SUITES." ""
for arch in i686 x86_64; do
    result="$dir/full-$arch"
    if [ -f "$result/done" ] && [ -f "$result/skipped" ]; then
        summarize "$arch had no suite to test." ""
        continue
    fi
    read_history "$result"
    if [ -f "$result/done" ] && [ -f "$result/failed" ] && [ ! -s "$result/failed" ] &&
        [ "$hstatus" = 0 ]; then
        summarize "$arch passed." ""
        continue
    fi
    rc=1
    if [ -f "$result/done" ] && [ -s "$result/failed" ]; then
        items=$(sort -u "$result/failed" | tr '\n' ' ' | sed 's/ $//')
    elif [ -f "$result/done" ] && [ -f "$result/failed" ]; then
        items=runner
    else
        items=unfinished
    fi
    for i in $items; do
        echo "$arch:$i"
    done >> "$file"
    summarize "$arch failed: $items." ""
    if [ -f "$result/err" ]; then
        lines=$(grep -E "$failing" "$result/err" | head -n 8)
        [ -z "$lines" ] || summarize '```' "$lines" '```' ""
    fi
    case $hstatus in
        1) summarize "The native verdict of the run:" '```' "$history" '```' "" ;;
        2) summarize "The native history of the run could not be read:" '```' "$history" '```' "" ;;
    esac
    [ -z "${GITHUB_RUN_ID:-}" ] ||
        summarize "Logs: \`gh run download $GITHUB_RUN_ID --repo ${GITHUB_REPOSITORY:-} --name full-$arch\`" ""
done
if [ "$rc" = 1 ]; then
    echo "ci: report: result: failed: $(tr '\n' ' ' < "$file" | sed 's/ $//')"
else
    echo "ci: report: result: passed on i686 and x86_64"
fi
exit "$rc"
