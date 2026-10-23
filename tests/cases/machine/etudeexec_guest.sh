#!/bin/sh
# etudeexec_guest.sh: build etude on Rach2 by its TinyCC recipe and run what
# it built: the version's program, and the test of the POSIX executor, built
# with the version's compiler and flags.
#
# Usage: sh etudeexec_guest.sh DIR
#
# The scenario of the etudeexec case (etudeexec_scenario.c) unpacks the files
# the build and the test need into DIR, laid out as the repository is, and
# runs this script there as root. Each result is a line `etudeexec: NAME: ok`
# or `etudeexec: NAME: fail: WHY`, and the last line is `etudeexec: end`. The
# version's cache and the test's scratch are under DIR.
set -u

dir=$1
cd "$dir" || exit 1
cache=$dir/cache

ok()
{
    echo "etudeexec: $1: ok"
}

bad()
{
    echo "etudeexec: $1: fail: $2"
}

# version NAME WORD: build or find the version of the source in DIR, and
# require the result WORD, `built` or `reused`; print the version's key.
version()
{
    out=$(sh tools/etude/provision_version.sh -l -C "$cache" 2>"$dir/$1.err")
    case $out in
        "etude provision: result: $2 "*)
            out=${out#"etude provision: result: $2 "}
            echo "${out%% *}" ;;
        *)
            sed 's/^/etudeexec: stderr: /' "$dir/$1.err" >&2
            return 1 ;;
    esac
}

if key=$(version built built); then
    ok built
else
    bad built 'provision_version.sh -l built no version'
fi
if again=$(version reused reused) && [ "$again" = "$key" ]; then
    ok reused
else
    bad reused 'a second request did not reuse the version'
fi

record=$cache/versions/$key
platform=$(sed -n 's/^platform //p' "$record" 2>/dev/null)
flags=$(sed -n 's/^flags //p' "$record" 2>/dev/null)
cc=$(sed -n 's/^cc //p' "$record" 2>/dev/null)
build=$(sed -n 's/^build //p' "$record" 2>/dev/null)
bin=$cache/$build/bin
case $platform in
    "rach2 $(uname -m)") ok platform ;;
    *) bad platform "the record gives '$platform'" ;;
esac
case $flags in
    *-D__rach2__*) ok recipe ;;
    *) bad recipe "the record's flags are '$flags'" ;;
esac

if "$bin/etude" check --selftest tools/etude/fixtures >"$dir/selftest.out" 2>&1; then
    ok check
else
    sed 's/^/etudeexec: check: /' "$dir/selftest.out"
    bad check 'etude check --selftest failed'
fi

schema=tools/etude/schemas/history.schema.json
if "$bin/etude" check history "$schema" tools/etude/schemas/history.example.jsonl >/dev/null 2>&1; then
    ok history
else
    bad history 'etude check history refused a whole stream'
fi
printf '%s' "$(cat tools/etude/schemas/history.example.jsonl)" >"$dir/cut.jsonl"
"$bin/etude" check history "$schema" "$dir/cut.jsonl" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
    ok history-lost
else
    bad history-lost "etude check history gave $status, not 2, for a lost last record"
fi

test=tests/host/etude_exec_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ')
extra=$(grep -oE 'host-cflags:[^*]*' "$test" | sed 's/host-cflags://' | tr '\n' ' ')
# shellcheck disable=SC2086 # the compiler, the flags and the sources are lists
if $cc $flags $extra -o "$dir/execcheck" "$test" $deps >"$dir/compile.out" 2>&1; then
    ok compile
else
    sed 's/^/etudeexec: compile: /' "$dir/compile.out"
    bad compile "$cc could not build $test"
fi

mkdir -p "$dir/tmp"
TMPDIR=$dir/tmp "$dir/execcheck" >"$dir/execcheck.out" 2>&1
status=$?
sed 's/^/etudeexec: execcheck: /' "$dir/execcheck.out"
if [ "$status" -ne 0 ]; then
    bad executor "the test exited $status"
elif grep -q 'not run' "$dir/execcheck.out"; then
    bad executor 'a row of the test did not run'
elif ! grep -q '^etude_exec_hosttest: pass$' "$dir/execcheck.out"; then
    bad executor 'the test did not say it passed'
else
    ok executor
fi
echo 'etudeexec: end'
