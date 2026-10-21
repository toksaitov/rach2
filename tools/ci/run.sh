#!/bin/sh
#
# tools/ci/run.sh: build the tree and run one of ci's sets for the
# architecture ARCH names, and keep what tools/ci/report.sh reads.
#
#   tools/ci/run.sh core DIR    the build, style, host and the core tier
#   tools/ci/run.sh full DIR    the build from clean and every suite of ARCH
#
# DIR receives `log`, the whole output; `failed`, one line for each step
# that failed: `build`, a suite's name, the set's own name when the runner
# failed without naming a suite of the registry, or `runner` when its
# history cannot show what ran; `kept/`, the serial logs that tests/run kept
# for the failed suites; and `done`, written last, without which a reader
# takes the run as unfinished. A full run first runs
# `make clean`, which removes ARCH's in-house objects and images but keeps
# the vendored builds, then `make`; a failure of either is `build`. It then
# runs every suite of the registry that runs on ARCH, `style` and `host`
# among them, as one set, and after it `bigmem`, which runs only on x86_64
# and needs the host alone, as a run of its own. When SUITES is set, as
# `ARCH:SUITE` words, a full run runs only the suites it names for ARCH, and
# refuses a word whose suite ARCH does not run; when it names none for ARCH,
# the run builds nothing and writes `skipped` beside `done`. The build runs with
# RACH2_MIN_FREE_GIB at 40 unless the environment gives another, so that it
# stops, as `build`, before it fills the host's disk. The exit status is 0 when
# nothing failed. A TERM to the run's process group, which timeout(1) sends
# at the workflow's time limit, ends the run without `done` once the step it
# interrupts has stopped, so that tests/run stops its machines before the
# next run starts; the status is then 143.
#
# `results` below says how this tree's tests/run gives its results. With
# `lines`, the runner's last `run: result: failed:` line on its standard
# error names the suites that failed, and a pass is its status 0.
# With `history`, the suites of a kind run as one run of tests/run, core's
# as `tests/run style host core` and full's with `bigmem` among them, since
# report.sh reads the history of one run. tests/run writes etude's history
# of it under DIR, which RACH2_RESULTS names: its streams in `history/` and
# the blobs they refer to, each named by its SHA-256, in `store/`, where
# report.sh reads them. `casecheck verdict` judges that history against the
# case manifests of tests/cases. The run passes only when the history shows
# every suite asked for planned whole and every attempt of every item
# passed, with its results in the store, and tests/run's status is 0. A
# suite with an item that did not pass fails, a failed retry among them, the
# set fails for a failed preparation, and `runner` for a history that is
# missing, cut off or inconsistent, or a status that disagrees with it.
#
# Each build and tests/run enters the installed site policy separately.
set -u


kind=${1-}
out=${2-}
case "$kind" in
    core|full) ;;
    *) echo "usage: tools/ci/run.sh core|full DIR" >&2; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: tools/ci/run.sh core|full DIR" >&2; exit 2; }
ARCH=${ARCH:-i686}
RACH2_MIN_FREE_GIB=${RACH2_MIN_FREE_GIB:-40}
export ARCH RACH2_MIN_FREE_GIB
# How this tree's tests/run gives its results, `lines` or `history`, as the
# header says.
results=lines
mkdir -p "$out/kept"
: > "$out/failed"
: > "$out/log"
: > "$out/start"

# The shell runs this trap only after the pipeline it waits for has ended.
trap 'echo "ci: $kind: stopped on $ARCH" | tee -a "$out/log"; exit 143' TERM

# Copy standard input to standard output and to FILE..., as tee does, but
# ignore TERM, so that what a stopping tests/run prints reaches the log and
# its writes do not end it by SIGPIPE before its machines are stopped. It
# runs only as an element of a pipeline, a subshell of its own, which it
# replaces with tee, so that the shell waits for tee itself.
copy() { # copy [-a] FILE...
    trap '' TERM
    exec tee "$@"
}

# Run a command with its output on the job's log and in DIR/log, and return
# the command's own status, which a pipe into tee would lose.
step() {
    { "$@" 2>&1; echo "$?" > "$out/rc"; } | copy -a "$out/log"
    return "$(cat "$out/rc")"
}

# Run tests/run with ARGS as one run, its history going to DIR/history and
# DIR/store, and append to DIR/failed the names of casecheck's verdict on
# that history, with `runner` when the verdict gave no result or tests/run's
# status disagrees with it, each name once; return 0 when both passed and 1
# otherwise. The suites asked for are ARGS, with `core` standing for the core
# tier's suites of ARCH, as tests/run takes it.
run_history() { # run_history NAME ARGS...
    name=$1; shift
    rm -rf "$out/history" "$out/store"
    mkdir -p "$out/history" "$out/store"
    { RACH2_RESULTS=$out tests/run "$@" 2>&1; echo "$?" > "$out/rc"; } |
        copy "$out/set.log" | copy -a "$out/log"
    rc=$(cat "$out/rc")
    suites=$(for s in "$@"; do
        if [ "$s" = core ]; then
            awk -F'|' -v a="$ARCH" '
                /^[a-z]/ && $3 == "core" && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
            ' tests/registry
        else
            echo "$s"
        fi
    done | awk '!seen[$0]++' | tr '\n' , | sed 's/,$//')
    { build/tools/etude/casecheck verdict -b "$out/store" -c "$(git rev-parse HEAD)" \
        "$ARCH" "$name" "$suites" "$out/history" 2>&1; echo "$?" > "$out/rc"; } |
        copy "$out/verdict" | copy -a "$out/log"
    verdict=$(cat "$out/rc")
    failed=$(sed -n 's/^casecheck: result: failed: //p' "$out/verdict")
    if [ "$verdict" = 0 ] && grep -qx 'casecheck: result: passed' "$out/verdict"; then
        [ "$rc" != 0 ] || return 0
        failed=runner
    elif [ "$verdict" != 1 ] || [ -z "$failed" ] || [ "$rc" = 0 ]; then
        failed="runner $failed"
    fi
    for f in $failed; do
        grep -qxF "$f" "$out/failed" || echo "$f" >> "$out/failed"
    done
    return 1
}

# Run tests/run with ARGS, and append to DIR/failed the suites it failed, or
# NAME when it failed without naming a suite of the registry; return 0 when it
# passed and 1 when it failed. The suites are on the runner's
# `run: result: failed: SUITE...` line, the last line of its standard error
# that begins `run: `. The launcher passes standard output and standard error
# through pipes of their own, so the two are not in order in the log; and the
# runner suite's output, printed before the summary, has lines of the same
# form from the runs it drives.
run_set() { # run_set NAME ARGS...
    name=$1; shift
    : > "$out/set.log"
    : > "$out/set.err"
    { { tests/run "$@" 2>&1 1>&3 3>&-; echo "$?" > "$out/rc"; } | copy "$out/set.err"; } 3>&1 |
        copy "$out/set.log" | copy -a "$out/log"
    rc=$(cat "$out/rc")
    [ "$rc" = 0 ] && return 0
    failed=$(grep '^run: ' "$out/set.err" | tail -n 1 |
        sed -n 's/^run: result: failed: *//p' | tr ' ' '\n' | sed '/^$/d')
    known=$(awk -F'|' '/^[a-z]/ { print $1 }' tests/registry)
    for f in $failed; do
        echo "$known" | grep -qx "$f" || failed=""
    done
    if [ -n "$failed" ]; then
        echo "$failed" >> "$out/failed"
    else
        echo "$name" >> "$out/failed"
    fi
    return 1
}

# Print each of NAMES, the suites ARCH runs, that SUITES names as an
# `ARCH:SUITE` word, or each of them when SUITES is empty; fail when SUITES
# names a suite of ARCH that is not among NAMES.
chosen() { # chosen NAME...
    [ -n "${SUITES:-}" ] || { printf '%s\n' "$@"; return 0; }
    for w in $SUITES; do
        case $w in
            "$ARCH":*) ;;
            *) continue ;;
        esac
        case " $* " in
            *" ${w#*:} "*) echo "${w#*:}" ;;
            *) return 1 ;;
        esac
    done
}

echo "ci: $kind: architecture $ARCH at $(git rev-parse --short HEAD)"
if [ "$kind" = full ]; then
    names=$(awk -F'|' -v a="$ARCH" '
        /^[a-z]/ && $1 != "bigmem" && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
    ' tests/registry)
    [ "$ARCH" != x86_64 ] || names="$names bigmem"
    # shellcheck disable=SC2086 # a suite name is one word by format
    names=$(chosen $names) || {
        echo "ci: full: refused: SUITES names a suite that $ARCH does not run: $SUITES" | copy -a "$out/log"
        exit 2
    }
    if [ -z "$names" ]; then
        echo "ci: full: no suite to run again on $ARCH" | copy -a "$out/log"
        : > "$out/skipped"
        : > "$out/done"
        exit 0
    fi
    [ -z "${SUITES:-}" ] ||
        echo "ci: full: running again on $ARCH: $(echo "$names" | tr '\n' ' ' | sed 's/ $//')" | copy -a "$out/log"
fi
if [ "$kind" = full ] && ! step make clean; then
    echo build >> "$out/failed"
elif ! step make; then
    echo build >> "$out/failed"
else
    case "$kind" in
        core)
            if [ "$results" = history ]; then
                run_history core style host core
            else
                run_set style style
                run_set host host
                run_set core core
            fi ;;
        full)
            rest=$(echo "$names" | grep -vx bigmem)
            if [ "$results" = history ]; then
                # shellcheck disable=SC2086 # a suite name is one word by format
                run_history all $names
            else
                # shellcheck disable=SC2086 # a suite name is one word by format
                [ -z "$rest" ] || run_set all $rest
                if echo "$names" | grep -qx bigmem; then
                    run_set bigmem bigmem
                fi
            fi ;;
    esac
fi
find build/tests/failed -mindepth 1 -maxdepth 1 -newer "$out/start" \
    -exec cp -R {} "$out/kept/" \; 2> /dev/null
: > "$out/done"
if [ -s "$out/failed" ]; then
    echo "ci: $kind: result: failed on $ARCH: $(tr '\n' ' ' < "$out/failed")"
    exit 1
fi
echo "ci: $kind: result: passed on $ARCH"
