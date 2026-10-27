#!/bin/sh
# Run the selected CI entry and preserve both architecture results.
# core selects style, host and core; full selects all suites, including bigmem.
# SUITES may select ARCH:SUITE operands for a full rerun. One history and store
# describe the whole collection; strict C verdicts project architecture results.
# Native entry requires an installed immutable ETUDE and version-two site.
# The lines entry remains active until native activation; history supports
# old-commit bisection fixtures.
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
# Both architectures, `ARCH='i686 x86_64'`, as the header says. ARCHES keeps
# the architectures of the run, and ARCH names those of each step.
ARCHES=$ARCH
both=""
case "$ARCH" in
    'i686 x86_64') both=1 ;;
    *' '*) echo "ci: $kind: refused: ARCH names one architecture, or both as 'i686 x86_64'" >&2; exit 2 ;;
esac
if [ "$kind" = full ]; then
    RACH2_STORE=
    export RACH2_STORE
fi
# Keep the existing entry until selected-site activation supplies the native
# policy, measured reference and retention root. Activation changes this one
# value to native; missing native configuration never selects a fallback.
results=lines
if [ "$results" = native ]; then
    program=${ETUDE:-}
    if [ -z "$program" ]; then
        site=${ETUDE_SITE:-}
        if [ -z "$site" ]; then
            case $(uname -s) in
                MINGW*|MSYS*|UCRT*|CLANG*|CYGWIN*) site=C:/ProgramData/rach2/etude-site ;;
                *) site=/etc/rach2/etude-site ;;
            esac
        fi
        program=$(sed -n 's/^etude //p' "$site") || exit 2
    fi
    case $program in /*|[A-Za-z]:/*) ;; *) echo 'ci: installed Etude is absent' >&2; exit 2 ;; esac
    exec "$program" run routine "$kind" "$out"
fi
top=$$
parent=$PPID
mkdir -p "$out/kept"
RACH2_FAILED=$out/kept
export RACH2_FAILED
: > "$out/failed"
: > "$out/log"
: > "$out/err"

# End the run without `done`, as stopped.
stop() {
    echo "ci: $kind: stopped on $ARCHES" | tee -a "$out/log"
    [ -z "$both" ] || share
    exit 143
}

# The shell runs this trap only after the pipeline it waits for has ended.
trap stop TERM

# Copy standard input to standard output and to FILE..., as tee does, but
# ignore TERM, so that what a stopping tests/run prints reaches the log and
# its writes do not end it by SIGPIPE before its machines are stopped. It
# runs only as an element of a pipeline, a subshell of its own, which it
# replaces with tee, so that the shell waits for tee itself.
copy() { # copy [-a] FILE...
    trap '' TERM
    exec tee "$@"
}

# Succeed while the run's shell is alive and its parent is the one it
# started under.
live() {
    # shellcheck disable=SC2046 # ps prints the two words read here
    set -- $(ps -o ppid=,stat= -p "$top" 2> /dev/null) Z
    [ "$1" = "$parent" ] && case $2 in Z*) false ;; esac
}

# Run COMMAND, as an element of a step's pipeline, and write its status to
# DIR/rc; start nothing when live fails. The shell removes DIR/rc before
# the step.
started() { # started COMMAND...
    live || return
    "$@"
    echo "$?" > "$out/rc"
}

# Set rc to the status the step wrote to DIR/rc. A step that wrote none, or
# whose command a signal ended, stops the run.
ended() {
    rc=$(cat "$out/rc" 2> /dev/null) || stop
    case $rc in
        '' | *[!0-9]*) stop ;;
    esac
    [ "$rc" -le 128 ] || stop
}

# Run a command with its output on the job's log and in DIR/log, and return
# the command's own status, which a pipe into tee would lose.
step() {
    rm -f "$out/rc"
    started "$@" 2>&1 | copy -a "$out/log"
    ended
    return "$rc"
}

# Run tests/run with ARGS as one run, its history going to DIR/history and
# DIR/store and its standard error to DIR/err as well, and append to DIR/failed
# the names of etude's verdict on that history, with `runner` when the verdict
# gave no result or tests/run's status disagrees with it, each name once; return
# 0 when both passed and 1 otherwise. The suites asked for are ARGS, with `core`
# standing for the core tier's suites of ARCH, as tests/run takes it.
run_history() { # run_history NAME ARGS...
    name=$1; shift
    rm -rf "$out/history" "$out/store"
    mkdir -p "$out/history" "$out/store"
    rm -f "$out/rc"
    { started env RACH2_RESULTS="$out" tests/run "$@" 2>&1 1>&3 3>&- |
        copy -a "$out/err"; } 3>&1 | copy "$out/set.log" | copy -a "$out/log"
    ended
    ran=$rc
    suites=$(for s in "$@"; do
        if [ "$s" = core ]; then
            awk -F'|' -v a="$ARCH" '
                /^[a-z]/ && $3 == "core" && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
            ' tests/registry
        else
            echo "$s"
        fi
    done | awk '!seen[$0]++' | tr '\n' , | sed 's/,$//')
    rm -f "$out/rc"
    started build/tools/etude/etude check verdict -b "$out/store" -c "$(git rev-parse HEAD)" \
        "$ARCH" "$name" "$suites" "$out/history" 2>&1 | copy "$out/verdict" | copy -a "$out/log"
    ended
    verdict=$rc
    rc=$ran
    failed=$(sed -n 's/^etude check: result: failed: //p' "$out/verdict")
    if [ "$verdict" = 0 ] && grep -qx 'etude check: result: passed' "$out/verdict"; then
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

# Run tests/run with ARGS, and append to DIR/failed the suites it failed, with
# `runner` for a fault of its own, or NAME when it failed without naming a
# suite of the registry or `runner`; return 0 when it passed and 1 when it
# failed. The suites are on the runner's `run: result: failed: SUITE...` line,
# the last line of its standard error that begins `run: `. The launcher passes
# standard output and standard error through pipes of their own, so the two
# are not in order in the log. With both architectures, tests/run runs on
# those ARCH names, its failed suites are `ARCH:SUITE` words, and each other
# name, `runner` and NAME among them, is written as `ARCH:NAME` for each
# architecture it ran on.
run_set() { # run_set NAME ARGS...
    name=$1; shift
    : > "$out/set.log"
    : > "$out/set.err"
    rm -f "$out/rc"
    { started tests/run "$@" 2>&1 1>&3 3>&- | copy "$out/set.err" |
        copy -a "$out/err"; } 3>&1 | copy "$out/set.log" | copy -a "$out/log"
    ended
    [ "$rc" = 0 ] && return 0
    failed=$(grep '^run: ' "$out/set.err" | tail -n 1 |
        sed -n 's/^run: result: failed: *//p' | tr ' ' '\n' | sed '/^$/d')
    known=$(awk -F'|' '/^[a-z]/ { print $1 } END { print "runner" }' tests/registry)
    for f in $failed; do
        echo "$known" | grep -qx "${f#*:}" || failed=""
    done
    [ -n "$failed" ] || failed=$name
    if [ -n "$both" ]; then
        failed=$(for f in $failed; do
            case $f in
                (*:*) echo "$f" ;;
                (*) for a in $ARCH; do echo "$a:$f"; done ;;
            esac
        done)
    fi
    echo "$failed" >> "$out/failed"
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

# The names of the suites of a full run of ARCH, `bigmem` last.
full_names() {
    awk -F'|' -v a="$ARCH" '
        /^[a-z]/ && $1 != "bigmem" && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
    ' tests/registry
    [ "$ARCH" != x86_64 ] || echo bigmem
}

# Give each architecture of a run of both its directory, DIR/KIND-ARCH, as a
# run of that architecture alone leaves it: the log; the standard error,
# without the lines of the other architecture's suites; the names that
# DIR/failed gives as `ARCH:NAME`; the kept logs of its cases, those of no
# architecture and those of no case going to both; and once the run has
# finished, `done`, beside `skipped` for one that had nothing to run.
share() { # share [finished]
    for a in i686 x86_64; do
        d=$out/$kind-$a
        o=x86_64
        [ "$a" = i686 ] || o=i686
        mkdir -p "$d/kept"
        cp "$out/log" "$d/log"
        grep -v "^run: $o:" "$out/err" > "$d/err"
        sed -n "s/^$a://p" "$out/failed" > "$d/failed"
        for k in "$out"/kept/*; do
            [ -e "$k" ] || continue
            case ${k##*/} in
                tests.$o.* | *.$o.r[0-9]*) ;;
                *) cp -R "$k" "$d/kept/" ;;
            esac
        done
        [ -n "${1:-}" ] || continue
        case " $tested " in *" $a "*) ;; *) : > "$d/skipped" ;; esac
        : > "$d/done"
    done
}

# Both architectures: each is cleaned for a full run, then each is built,
# and the suites of every architecture that built run as one run of tests/run,
# `bigmem` after it as a run of its own; a failure is `ARCH:build`. With
# SUITES, a full run runs `ARCH:SUITE` for each word, and an architecture
# that SUITES names nothing of is not built and is `skipped`.
if [ -n "$both" ]; then
    [ "$results" = lines ] || {
        echo "ci: $kind: refused: a run of both architectures reads the runner's lines" | copy -a "$out/log"
        exit 2
    }
    echo "ci: $kind: architectures i686 and x86_64 at $(git rev-parse --short HEAD)"
    tested=""
    words=""
    for a in i686 x86_64; do
        ARCH=$a
        if [ "$kind" = full ]; then
            # shellcheck disable=SC2046 # a suite name is one word by format
            names=$(chosen $(full_names)) || {
                echo "ci: full: refused: SUITES names a suite that $a does not run: $SUITES" | copy -a "$out/log"
                exit 2
            }
            [ -n "$names" ] || { echo "ci: full: no suite to run again on $a" | copy -a "$out/log"; continue; }
            [ -z "${SUITES:-}" ] ||
                echo "ci: full: running again on $a: $(echo "$names" | tr '\n' ' ' | sed 's/ $//')" | copy -a "$out/log"
            for n in $names; do words="$words $a:$n"; done
        fi
        tested="$tested $a"
    done
    built=""
    for a in $tested; do
        ARCH=$a
        [ "$kind" != full ] || step make clean || echo "$a:build" >> "$out/failed"
    done
    for a in $tested; do
        ARCH=$a
        grep -qx "$a:build" "$out/failed" || step make || echo "$a:build" >> "$out/failed"
        grep -qx "$a:build" "$out/failed" || built="$built $a"
    done
    ARCH=${built# }
    if [ -n "$built" ] && [ "$kind" = core ]; then
        run_set style style
        run_set host host
        run_set core core
    elif [ -n "$built" ]; then
        rest=""
        bigmem=""
        for w in $words; do
            case " $built " in
                *" ${w%%:*} "*) ;;
                *) continue ;;
            esac
            if [ "$w" = x86_64:bigmem ]; then
                bigmem=1
            else
                rest="$rest $w"
            fi
        done
        [ -n "${SUITES:-}" ] || rest=$(for w in $rest; do echo "${w#*:}"; done | awk '!seen[$0]++')
        # shellcheck disable=SC2086 # a suite name is one word by format
        [ -z "$rest" ] || run_set all $rest
        if [ -n "$bigmem" ]; then
            ARCH=x86_64
            run_set bigmem bigmem
        fi
    fi
    share finished
    : > "$out/done"
    if [ -s "$out/failed" ]; then
        echo "ci: $kind: result: failed: $(tr '\n' ' ' < "$out/failed")"
        exit 1
    fi
    echo "ci: $kind: result: passed on i686 and x86_64"
    exit 0
fi

echo "ci: $kind: architecture $ARCH at $(git rev-parse --short HEAD)"
if [ "$kind" = full ]; then
    # shellcheck disable=SC2046 # a suite name is one word by format
    names=$(chosen $(full_names)) || {
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
: > "$out/done"
if [ -s "$out/failed" ]; then
    echo "ci: $kind: result: failed on $ARCH: $(tr '\n' ' ' < "$out/failed")"
    exit 1
fi
echo "ci: $kind: result: passed on $ARCH"
