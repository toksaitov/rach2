#!/usr/bin/env sh
#
# dbcheck.sh: every in-house C source is compiled by something.
#
# tools/dev/gen_compile_commands.sh writes one database per unit under
# build/compile_commands/. A source none of them names is one no build
# compiles, or one the generator missed; either is a fault, so this fails
# naming it. The sources are the in-house trees: the kernel, the KRFFS tools,
# the utilities, the shell, the tests with their host and musl programs, the
# tools, and Doom's platform layer. The vendored trees are not in-house.
#
# Two kinds of source are compiled without an entry of their own. A fragment
# that another source includes by name, as `ish/ish_syscalls.c` includes
# `ish_syscalls.i386.c`, is compiled as part of that source's entry. A
# fixture under tests/system/fixtures is compiled by the test script that
# names it, against a work copy of the source it tests and with the flags the
# script sets, so no fixed command describes it; such a source passes when a
# script under tests/system names it.
#
# Usage: dbcheck.sh [DBDIR]
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DBDIR="${1:-$REPO/build/compile_commands}"
cd "$REPO" || exit 1
if ! ls "$DBDIR"/*.json >/dev/null 2>&1; then
    echo "dbcheck: no database under $DBDIR; run tools/dev/gen_compile_commands.sh" >&2
    exit 1
fi
# The absolute path of every entry: its directory joined with its file, a
# file that is already absolute taken as it is. The generator writes one
# entry per line, and this reads that shape.
known=$(mktemp) || exit 2
trap 'rm -f "$known"' EXIT INT TERM
cat "$DBDIR"/*.json | LC_ALL=C awk -F'"' '
    /"directory"/ {
        for (i = 1; i <= NF; i++) {
            if ($i == "directory") d = $(i + 2)
            if ($i == "file") f = $(i + 2)
        }
        if (f ~ /^\//) print f; else print d "/" f
    }' | sed 's#/\./#/#g' | sort -u >"$known"
sources=$( {
    find rach2 -name '*.c'
    find krffs utils ish tests -path '*/vendor' -prune -o -path '*/vs-dokany' -prune -o -name '*.c' -print
    find tools -name '*.c'
    find ports/doom -maxdepth 1 -name '*.c'
} 2>/dev/null | sort -u )
included() {
    grep -rqF --include='*.c' "#include \"${1##*/}\"" "$(dirname "$1")"
}
fixture() {
    case $1 in tests/system/fixtures/*) ;; *) return 1 ;; esac
    grep -rqF --include='*.sh' "fixtures/${1##*/}" tests/system
}
missing=0
for f in $sources; do
    grep -qxF "$REPO/$f" "$known" && continue
    included "$f" && continue
    fixture "$f" && continue
    echo "dbcheck: $f is in no compilation database"
    missing=$((missing + 1))
done
if [ "$missing" -gt 0 ]; then
    echo "dbcheck: $missing source(s) in no database" >&2
    exit 1
fi
echo "dbcheck: ok: $(echo "$sources" | wc -l | tr -d ' ') sources, each compiled by a build or a test"
