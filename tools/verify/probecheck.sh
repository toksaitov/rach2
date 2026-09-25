#!/bin/sh
# Require every built guest program to have a suite or a documented helper use.
set -eu
REPO=${PROBECHECK_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/probecheck.XXXXXX")
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf "$WORK"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
make -s --no-print-directory -C "$REPO/tests" -f Makefile -f - probecheck-list > "$WORK/built" <<'MK'
.PHONY: probecheck-list
probecheck-list:
	@printf '%s\n' $(PROGS)
MK
for source in "$REPO"/tests/musl/*.c; do
    [ -f "$source" ] || continue
    printf 'musl/%s\n' "$(basename "$source" .c)" >> "$WORK/built"
done
awk '
    /PROBES\[\].*= *\{/ { probes = 1 }
    probes && /^[ \t]*"/ {
        n = split($0, fields, "\"")
        for (i = 2; i < n; i += 2) print fields[i], "default"
    }
    probes && /};/ { probes = 0 }
' "$REPO/tests/system/runner/runtests.c" > "$WORK/default"
awk '
    function fail(message) { print "probecheck: " message > "/dev/stderr"; bad = 1 }
    FILENAME == ARGV[1] { if ($1 != "") built[$1] = 1; next }
    FILENAME == ARGV[2] {
        if ($0 !~ /^#/ && $0 != "") { split($0, fields, "|"); suite[fields[1]] = 1 }
        next
    }
    /^#/ || /^$/ { next }
    {
        if (!($1 in built)) fail("unbuilt program: " $1)
        if (assigned[$1]++) fail("duplicate assignment: " $1)
        if (FILENAME == ARGV[5]) {
            if (NF < 2) fail("helper has no reason: " $1)
        } else if (NF != 2 || !($2 in suite)) fail("unknown suite for " $1 ": " $2)
    }
    END {
        for (name in built) if (!(name in assigned)) fail("unassigned program: " name)
        if (!bad) print "probecheck: passed: every built program has a suite or helper use"
        exit bad
    }
' "$WORK/built" "$REPO/tests/registry" "$WORK/default" \
    "$REPO/tests/probe-suites" "$REPO/tests/probe-helpers"
finished=1
