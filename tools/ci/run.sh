#!/bin/sh
# Run every suite of both architectures, then the i686 cases on the x86-64
# kernel, the ia32 profile, or only the ARCH:SUITE and ia32:SUITE words of
# SUITES, with the in-house code built from clean, or built incrementally
# when SUITES runs some again, and the vendor and GNU builds taken from the
# host's step store, and keep each one's results. RACH2_RERUN_CASES, case
# identities, limits each run of tests/run to those of its suites.
# Native entry requires an installed immutable ETUDE and version-two site.
# The lines entry remains active until native activation.
set -u


kind=${1-}
out=${2-}
[ "$kind" = full ] && [ -n "$out" ] || { echo "usage: tools/ci/run.sh full DIR" >&2; exit 2; }
ARCH=${ARCH:-i686 x86_64}
RACH2_MIN_FREE_GIB=${RACH2_MIN_FREE_GIB:-40}
export ARCH RACH2_MIN_FREE_GIB
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
[ "$ARCH" = 'i686 x86_64' ] || {
    echo "ci: full: refused: ARCH is 'i686 x86_64', both architectures" >&2
    exit 2
}
# ARCHES keeps the architectures of the run, and ARCH names those of each step.
ARCHES=$ARCH
top=$$
parent=$PPID
mkdir -p "$out/kept"
RACH2_FAILED=$out/kept
export RACH2_FAILED
: > "$out/failed"
: > "$out/log"
: > "$out/err"
: > "$out/err-ia32"
# The file that run_set copies tests/run's standard error to.
errs=$out/err

# End the run without `done`, as stopped.
stop() {
    echo "ci: full: stopped on $ARCHES" | tee -a "$out/log"
    share
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

# Run tests/run with ARGS, or set_runner's command with them, and append to
# DIR/failed the suites it failed, with `runner` for a fault of its own, or
# NAME when it failed without naming a suite of the registry or `runner`;
# return 0 when it passed and 1 when it failed. The suites are on the
# runner's `run: result: failed: SUITE...` line, the last line of its
# standard error that begins `run: `. The launcher passes standard output and
# standard error through pipes of their own, so the two are not in order in
# the log. tests/run runs on the ARCH names, its failed suites are
# `ARCH:SUITE` words, and each other name, `runner` and NAME among them, is
# written as `ARCH:NAME` for each architecture it ran on. Under
# RACH2_KERNEL=ia32 each is written as `ia32:NAME` instead.
run_set() { # run_set NAME ARGS...
    name=$1; shift
    rerun_cases "$@"
    : > "$out/set.log"
    : > "$out/set.err"
    rm -f "$out/rc"
    # shellcheck disable=SC2086 # set_runner is a command and its words
    { started ${set_runner:-tests/run} "$@" 2>&1 1>&3 3>&- | copy "$out/set.err" |
        copy -a "$errs"; } 3>&1 | copy "$out/set.log" | copy -a "$out/log"
    ended
    [ "$rc" = 0 ] && return 0
    failed=$(grep '^run: ' "$out/set.err" | tail -n 1 |
        sed -n 's/^run: result: failed: *//p' | tr ' ' '\n' | sed '/^$/d')
    known=$(awk -F'|' '/^[a-z]/ { print $1 } END { print "runner" }' tests/registry)
    for f in $failed; do
        echo "$known" | grep -qx "${f#*:}" || failed=""
    done
    [ -n "$failed" ] || failed=$name
    for f in $failed; do
        case ${RACH2_KERNEL:-}:$f in
            (ia32:*) echo "ia32:${f#*:}" ;;
            (*:*:*) echo "$f" ;;
            (*) for a in $ARCH; do echo "$a:$f"; done ;;
        esac
    done >> "$out/failed"
    return 1
}

# Set RACH2_CASES to the identities of RACH2_RERUN_CASES that belong to the
# suites of WORDS, SUITE or ARCH:SUITE, a case identity's second part being
# its suite, when they name at least one case of each; otherwise unset it, so
# that the suites run whole. The ia32 profile runs its suites whole, since
# the identities do not say which architecture's run failed and etude refuses
# a chosen case that does not run under the profile.
rerun_cases() { # rerun_cases WORD...
    unset RACH2_CASES
    [ -n "${RACH2_RERUN_CASES:-}" ] || return 0
    [ "${RACH2_KERNEL:-}" != ia32 ] || return 0
    rc_ids=""
    for w in "$@"; do
        # shellcheck disable=SC2086 # the identities are words
        rc_one=$(printf '%s\n' $RACH2_RERUN_CASES | awk -F. -v s="${w#*:}" '$2 == s')
        [ -n "$rc_one" ] || return 0
        rc_ids="$rc_ids $rc_one"
    done
    # shellcheck disable=SC2086 # the identities are words
    RACH2_CASES=$(printf '%s\n' $rc_ids | awk '!seen[$0]++' | tr '\n' ' ' | sed 's/ $//')
    export RACH2_CASES
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

# Give each architecture its directory, DIR/full-ARCH, as a run of that
# architecture alone leaves it: the log; the standard error, without the
# lines of the other architecture's suites; the names that DIR/failed gives
# as `ARCH:NAME`; the kept logs of its cases, those of no architecture and
# those of no case going to both; and once the run has finished, `done`,
# beside `skipped` for one that had nothing to run. The ia32 profile's
# directory, DIR/full-ia32, has the log, the profile's own standard error and
# kept logs, and the names that DIR/failed gives as `ia32:NAME`.
share() { # share [finished]
    for a in i686 x86_64 ia32; do
        d=$out/full-$a
        mkdir -p "$d/kept"
        cp "$out/log" "$d/log"
        sed -n "s/^$a://p" "$out/failed" > "$d/failed"
        if [ "$a" = ia32 ]; then
            cp "$out/err-ia32" "$d/err"
            for k in "$out"/kept-ia32/*; do
                [ ! -e "$k" ] || cp -R "$k" "$d/kept/"
            done
        else
            o=x86_64
            [ "$a" = i686 ] || o=i686
            grep -v "^run: $o:" "$out/err" > "$d/err"
            for k in "$out"/kept/*; do
                [ -e "$k" ] || continue
                case ${k##*/} in
                    tests.$o.* | *.$o.r[0-9]*) ;;
                    *) cp -R "$k" "$d/kept/" ;;
                esac
            done
        fi
        [ -n "${1:-}" ] || continue
        case " $tested " in *" $a "*) ;; *) : > "$d/skipped" ;; esac
        : > "$d/done"
    done
}

# Each architecture is cleaned, then each is built, and the suites of every
# architecture that built run as one run of tests/run under the site's
# accelerator, those tools/ci/kvm-suites names after it as one run under KVM,
# `bigmem` after them as a run of its own, and last, once both architectures
# have built, the ia32 profile's suites, as tools/ci/ia32-suites and
# ia32-deferred give them, as a run of `RACH2_KERNEL=ia32 ARCH=i686 tests/run`,
# which boots the i686 build's cases on the x86-64 build's kernel
# (tests/Readme.md, The ia32 profile); a failure is `ARCH:build`, and
# `ia32:build` when the profile could not run for want of a build. With SUITES,
# the run runs `ARCH:SUITE` for each word and the SUITE of each `ia32:SUITE`
# word under the profile. An architecture that SUITES names nothing of is
# `skipped`, and is not built unless the profile needs it; the profile is
# `skipped` when SUITES names nothing of it.
echo "ci: full: architectures i686 and x86_64 at $(git rev-parse --short HEAD)"
tested=""
words=""
for a in i686 x86_64; do
    ARCH=$a
    # shellcheck disable=SC2046 # a suite name is one word by format
    names=$(chosen $(full_names)) || {
        echo "ci: full: refused: SUITES names a suite that $a does not run: $SUITES" | copy -a "$out/log"
        exit 2
    }
    [ -n "$names" ] || { echo "ci: full: no suite to run again on $a" | copy -a "$out/log"; continue; }
    [ -z "${SUITES:-}" ] ||
        echo "ci: full: running again on $a: $(echo "$names" | tr '\n' ' ' | sed 's/ $//')" | copy -a "$out/log"
    for n in $names; do words="$words $a:$n"; done
    tested="$tested $a"
done
# The suites of the ia32 profile: `all`, or each that an `ia32:SUITE` word of
# SUITES names, which must be one of a full run of i686.
ia32=""
if [ -z "${SUITES:-}" ]; then
    ia32=all
else
    ARCH=i686
    names=$(full_names)
    for w in $SUITES; do
        case $w in
            ia32:*) ;;
            *) continue ;;
        esac
        echo "$names" | grep -qx "${w#ia32:}" || {
            echo "ci: full: refused: SUITES names a suite that i686 does not run: $SUITES" | copy -a "$out/log"
            exit 2
        }
        ia32="$ia32 ${w#ia32:}"
    done
    ia32=${ia32# }
    [ -z "$ia32" ] || echo "ci: full: running again on ia32: $ia32" | copy -a "$out/log"
fi
# The profile runs the suites tools/ci/ia32-suites names in place of `all`,
# and leaves out those tools/ci/ia32-deferred names; etude refuses a named
# suite that has no case under the profile.
if [ -n "$ia32" ]; then
    deferred=$(sed -e 's/#.*//' -e '/^ *$/d' tools/ci/ia32-deferred)
    [ "$ia32" != all ] || ia32=$(sed -e 's/#.*//' -e '/^ *$/d' tools/ci/ia32-suites)
    kept=""
    for n in $ia32; do
        echo "$deferred" | grep -qx "$n" || kept="$kept $n"
    done
    ia32=${kept# }
    echo "ci: full: ia32 leaves out: $(echo "$deferred" | tr '\n' ' ')" | copy -a "$out/log"
fi
builds=$tested
[ -z "$ia32" ] || builds="i686 x86_64"
built=""
# A run of some suites again builds on what the last run built.
for a in $builds; do
    ARCH=$a
    [ -n "${SUITES:-}" ] || step make clean || echo "$a:build" >> "$out/failed"
done
for a in $builds; do
    ARCH=$a
    grep -qx "$a:build" "$out/failed" || step make || echo "$a:build" >> "$out/failed"
    grep -qx "$a:build" "$out/failed" || built="$built $a"
done
# The architectures whose suites run: those of `tested` that built.
ARCH=""
for a in $built; do
    case " $tested " in
        *" $a "*) ARCH="$ARCH $a" ;;
    esac
done
ARCH=${ARCH# }
if [ -n "$ARCH" ]; then
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
    # The suites of tools/ci/kvm-suites leave the site's run for one of their
    # own under KVM, and those of tools/ci/loaded-suites for one after it,
    # beside loaded.py's fixed load on Linux.
    kvm=""
    loaded=""
    if [ -f tools/ci/kvm-suites ]; then
        listed=$(sed -e 's/#.*//' -e '/^ *$/d' tools/ci/kvm-suites)
        heavy=""
        if [ "$(uname -s)" = Linux ] && [ -f tools/ci/loaded-suites ]; then
            heavy=$(sed -e 's/#.*//' -e '/^ *$/d' tools/ci/loaded-suites)
        fi
        others=""
        for w in $rest; do
            if [ -n "$heavy" ] && echo "$heavy" | grep -qx "${w#*:}"; then
                loaded="$loaded $w"
            elif echo "$listed" | grep -qx "${w#*:}"; then
                kvm="$kvm $w"
            else
                others="$others $w"
            fi
        done
        rest=$others
    fi
    # shellcheck disable=SC2086 # a suite name is one word by format
    [ -z "$rest" ] || run_set all $rest
    if [ -n "$kvm" ]; then
        accel=${RACH2_QEMU_ACCEL-}
        RACH2_QEMU_ACCEL=kvm
        export RACH2_QEMU_ACCEL
        # shellcheck disable=SC2086 # a suite name is one word by format
        run_set all $kvm
        RACH2_QEMU_ACCEL=$accel
    fi
    if [ -n "$loaded" ]; then
        accel=${RACH2_QEMU_ACCEL-}
        RACH2_QEMU_ACCEL=kvm
        export RACH2_QEMU_ACCEL
        set_runner="python3 tests/graphics/browser/loaded.py $out/load -- tests/run -j1 -C4"
        # shellcheck disable=SC2086 # a suite name is one word by format
        run_set all $loaded
        set_runner=""
        RACH2_QEMU_ACCEL=$accel
    fi
    if [ -n "$bigmem" ]; then
        ARCH=x86_64
        run_set bigmem bigmem
    fi
fi
if [ -n "$ia32" ]; then
    tested="$tested ia32"
    if [ "${built# }" = 'i686 x86_64' ]; then
        ARCH=i686
        RACH2_KERNEL=ia32
        RACH2_FAILED=$out/kept-ia32
        export RACH2_KERNEL RACH2_FAILED
        mkdir -p "$RACH2_FAILED"
        errs=$out/err-ia32
        # shellcheck disable=SC2086 # a suite name is one word by format
        run_set all $ia32
    else
        echo ia32:build >> "$out/failed"
    fi
fi
share finished
: > "$out/done"
if [ -s "$out/failed" ]; then
    echo "ci: full: result: failed: $(tr '\n' ' ' < "$out/failed")"
    exit 1
fi
echo "ci: full: result: passed on i686 and x86_64${ia32:+, and on ia32}"
exit 0
