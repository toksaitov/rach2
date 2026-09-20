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
# --no-print-directory it must not. `--mutate` puts the first-word test back in
# the private copy of the rule and requires the --no-print-directory row to
# fail, then restores it and requires every row to pass again.
#
# Usage:
#     sh tests/system/dryrun_test.sh [--mutate]
#
set -eu

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    *) echo 'dryrun-test: usage: dryrun_test.sh [--mutate]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$REPO"
mkdir -p build
WORK=$(mktemp -d "$REPO/build/dryrun-test.XXXXXX")
trap 'rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP

fails=0
say() { echo "dryrun-test: $1"; }
bad() { echo "dryrun-test: FAIL: $1" >&2; fails=$((fails + 1)); }

mkdir -p "$WORK/mk"
cp mk/dryrun.mk "$WORK/mk/dryrun.mk"
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

rows()
{
    row 'no flags' built
    row '--no-print-directory' built --no-print-directory
    row '-n' skipped -n
    row '--dry-run' skipped --dry-run
    row '-n with --no-print-directory' skipped -n --no-print-directory
}

if [ "$mutate" -eq 0 ]; then
    rows
    [ "$fails" = 0 ] || exit 1
    say PASS
    exit 0
fi

rows
[ "$fails" = 0 ] || { echo 'dryrun-test: FAIL: the fixed rule did not pass' >&2; exit 1; }
say 'the fixed rule passes every row'

# The mutation is the rule this entry was written for: the first word of
# MAKEFLAGS searched for the letter, which reads --no-print-directory as -n.
cat > "$WORK/mk/dryrun.mk" <<'EOD'
DRY_RUN := $(findstring n,$(firstword $(MAKEFLAGS)))
EOD
fails=0
rm -f "$WORK/marker"
out=$( (cd "$WORK" && make --no-print-directory stamp) 2>&1 ) || true
case "$out" in
    *'dry run'*) say 'the first-word rule skips the build under --no-print-directory: the mutation is caught' ;;
    *) echo 'dryrun-test: FAIL: the mutation built anyway' >&2; exit 1 ;;
esac
[ -f "$WORK/marker" ] && { echo 'dryrun-test: FAIL: the mutation built the stamp' >&2; exit 1; }

cp mk/dryrun.mk "$WORK/mk/dryrun.mk"
touch "$WORK/mk/dryrun.mk"
rows
[ "$fails" = 0 ] || { echo 'dryrun-test: FAIL: the restored rule did not pass' >&2; exit 1; }
say 'restored: PASS'
