#!/bin/sh
#
# tools/ci/run.sh: build the tree and run one of ci's sets for the
# architecture ARCH names, and keep what tools/ci/report.sh reads.
#
#   tools/ci/run.sh core DIR    the build, style, host and the core tier
#   tools/ci/run.sh full DIR    the build from clean and every suite of ARCH
#
# DIR receives `log`, the whole output; `failed`, one line for each step
# that failed: `build`, a suite's name, or the set's own name when the
# runner failed without naming a suite of the registry; `kept/`, the serial
# logs that tests/run kept for the failed suites; and `done`, written last,
# without which a reader takes the run as unfinished. A full run first runs
# `make clean`, which removes ARCH's in-house objects and images but keeps
# the vendored builds, then `make`; a failure of either is `build`. It then
# runs every suite of the registry that runs on ARCH, `style` and `host`
# among them, as one set, and after it `bigmem`, which runs only on x86_64
# and needs the host alone, as a run of its own. The exit status is 0 when
# nothing failed. A TERM to the run's process group, which timeout(1) sends
# at the workflow's time limit, ends the run without `done` once the step it
# interrupts has stopped, so that tests/run stops its machines before the
# next run starts; the status is then 143.
#
set -u

kind=${1-}
out=${2-}
case "$kind" in
    core|full) ;;
    *) echo "usage: tools/ci/run.sh core|full DIR" >&2; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: tools/ci/run.sh core|full DIR" >&2; exit 2; }
ARCH=${ARCH:-i686}
export ARCH
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

# Run tests/run with ARGS, and append to DIR/failed the suites it failed, or
# NAME when it failed without naming a suite of the registry; return 0 when it
# passed and 1 when it failed. The suites are on the runner's own
# `run: result: failed: SUITE...` lines, which follow the last suite's result;
# the runner suite's output, printed before it, has lines of the same form
# from the runs it drives.
run_set() { # run_set NAME ARGS...
    name=$1; shift
    : > "$out/set.log"
    { tests/run "$@" 2>&1; echo "$?" > "$out/rc"; } | copy "$out/set.log" | copy -a "$out/log"
    rc=$(cat "$out/rc")
    [ "$rc" = 0 ] && return 0
    failed=$(awk '
        /^run: [a-z0-9-]+: (PASS|FAIL) in / { last = NR }
        { line[NR] = $0 }
        END {
            for (i = last + 1; i <= NR; i++)
                if (sub(/^run: result: failed: */, "", line[i]))
                    print line[i]
        }' "$out/set.log" | tr ' ' '\n' | sed '/^$/d')
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

echo "ci: $kind: architecture $ARCH at $(git rev-parse --short HEAD)"
if [ "$kind" = full ] && ! step make clean; then
    echo build >> "$out/failed"
elif ! step make; then
    echo build >> "$out/failed"
else
    case "$kind" in
        core)
            run_set style style
            run_set host host
            run_set core core ;;
        full)
            names=$(awk -F'|' -v a="$ARCH" '
                /^[a-z]/ && $1 != "bigmem" && ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
            ' tests/registry)
            # shellcheck disable=SC2086 # a suite name is one word by format
            run_set all $names
            if [ "$ARCH" = x86_64 ]; then
                run_set bigmem bigmem
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
