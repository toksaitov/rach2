#!/usr/bin/env sh
#
# dryrun_test.sh: the vendor stamps' dry-run guard, under the flags the tree
# passes. `tests/run` invokes make with `--no-print-directory`, and a guard
# that reads that as `-n` skips a vendor build whose outputs are absent, after
# which the suite fails on what was never built.
#
# The rows run a recipe of the vendor stamps' shape, in a private tree, over
# the real mk/dryrun.mk: with no flags and with --no-print-directory the build
# must run, and with -n, with --dry-run, and with -n beside
# --no-print-directory it must not. DRYRUN_MK names the rule to copy, by
# default the tree's; tests/proofs/dryrun.json runs the rows against a copy
# with the first-word test put back.
#
# Usage:
#     sh tests/system/dryrun_test.sh
#
set -eu

[ "$#" -eq 0 ] || { echo 'dryrun-test: usage: dryrun_test.sh' >&2; exit 2; }

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$REPO/tests/system/runlock.sh"
runlock_hold "$REPO" dryrun-test "$REPO/tests/system/dryrun_test.sh" "$@"
cd "$REPO"
mkdir -p build
WORK=
trap '[ -z "$WORK" ] || rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "$REPO/build/dryrun-test.XXXXXX")

fails=0
# say TEXT: print a progress line.
say() { echo "dryrun-test: $1"; }
# bad TEXT: report a failed row and count it in `fails`.
bad() { echo "dryrun-test: FAIL: $1" >&2; fails=$((fails + 1)); }

mkdir -p "$WORK/mk"
cp "${DRYRUN_MK:-mk/dryrun.mk}" "$WORK/mk/dryrun.mk"
cat > "$WORK/build.sh" <<'EOB'
#!/bin/sh
echo built > "$(dirname "$0")/marker"
echo "stamp: result: built"
EOB
chmod +x "$WORK/build.sh"
cat > "$WORK/Makefile" <<'EOM'
include mk/dryrun.mk

.PHONY: stamp
stamp:
	+@if [ -n "$(DRY_RUN)" ]; then echo "stamp: dry run"; else ./build.sh; fi
EOM

# One row: the flags, whether the build must run, and what make prints.
row()
{
    label=$1
    want=$2
    shift 2
    rm -f "$WORK/marker"
    out=$( (cd "$WORK" && make "$@" stamp) 2>&1 ) || {
        bad "$label: make failed: $out"
        return
    }
    if [ "$want" = built ]; then
        [ -f "$WORK/marker" ] || { bad "$label: the build did not run"; return; }
        case "$out" in
            *'dry run'*) bad "$label: make called it a dry run"; return ;;
        esac
    else
        [ -f "$WORK/marker" ] && { bad "$label: the build ran under a dry run"; return; }
        case "$out" in
            *'dry run'*) ;;
            *) bad "$label: make did not call it a dry run"; return ;;
        esac
    fi
    say "$label: $want: OK"
}

# Run the five rows.
rows()
{
    row 'no flags' built
    row '--no-print-directory' built --no-print-directory
    row '-n' skipped -n
    row '--dry-run' skipped --dry-run
    row '-n with --no-print-directory' skipped -n --no-print-directory
}

rows
[ "$fails" = 0 ] || exit 1
say PASS
