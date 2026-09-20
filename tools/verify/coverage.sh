#!/usr/bin/env sh
#
# coverage.sh [NAME...]: the host tier's coverage, as an inventory and not a
# score. Each host test named (all of them without arguments) is built again
# with --coverage under the tier's own flags, run, and gcov is asked about
# the kernel sources the test's `host-deps:` lines name: the lines executed
# and the branches taken, per file. Nothing is summed across files or
# tests. A person reads the report to choose the next test from a branch
# nothing took, and to notice the drop when a case is removed. It is never
# kernel coverage: the tier substitutes the allocator and the string
# routines and runs at the host's width, and the kernel's proof is a boot.
#
# On the Mac `gcov` is llvm-cov's, which prints the same summary lines.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1
COV=/tmp/rach2_verify/coverage
GCOV="${GCOV:-gcov}"
rm -rf "$COV"
mkdir -p "$COV"
HOST_PRINT_COMMANDS=1 sh tests/system/suites/host.sh 2>/dev/null | grep "_hosttest" >"$COV/commands"
if [ $# -eq 0 ]; then
    while read -r n; do
        set -- "$@" "$n"
    done <<NAMES
$(sed 's/.*tests\/host\/\([a-z0-9_]*\)_hosttest\.c.*/\1/' "$COV/commands")
NAMES
fi
fail=0
for name in "$@"; do
    cmd=$(grep " tests/host/${name}_hosttest.c" "$COV/commands")
    if [ -z "$cmd" ]; then
        echo "coverage: no host test named $name" >&2; fail=1; continue
    fi
    dir="$COV/$name"
    mkdir -p "$dir"
    # The tier's command, with coverage on and its output in the test's own
    # directory: the sources are given by their repo-relative names, which
    # gcov reads back through the object's recorded working directory.
    cmd=$(echo "$cmd" | sed "s# -o [^ ]* # --coverage -o $dir/test #")
    if ! (cd "$REPO" && $cmd) >"$dir/build.log" 2>&1; then
        echo "coverage: $name: did not build; see $dir/build.log" >&2; fail=1; continue
    fi
    (cd "$REPO" && "$dir/test") >"$dir/run.log" 2>&1
    echo "$name: exit $?"
    # Every kernel source the test compiled, whether linked from its
    # host-deps or included into the test itself: gcov reports a section
    # per source file, and the ones under rach2/ are the inventory.
    for gcno in "$dir"/test-*.gcno; do
        obj=${gcno%.gcno}
        $GCOV -n -b -o "$obj" "$obj" 2>/dev/null | awk '
            /^File/ { keep = index($0, "rach2/") > 0; if (keep) { f = $0; sub(/^File .rach2\//, "rach2/", f); sub(/.$/, "", f) } }
            keep && /Lines executed/ { l = $0; sub(/Lines executed:/, "", l) }
            keep && /Taken at least once/ { t = $0; sub(/Taken at least once:/, "", t); print "  " f ": lines " l ", branches taken " t }'
    done | sort -u
done
exit $fail
