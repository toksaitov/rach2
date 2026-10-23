#!/bin/sh
# win-run-smoke.sh: check the shell entry points with native Windows helpers.
# Run in an admitted MSYS2 shell with UCRT64 GCC available.
set -eu
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*) ;; *) exit 2 ;; esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
finished=
status=1
trap 'status=$?; [ -z "$work" ] || rm -rf "$work"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$repo/build/tests"
work=$(mktemp -d "$repo/build/tests/rach2-win-run.XXXXXX")
sh "$repo/tests/run" inventory > "$work/inventory"
cmp "$repo/tests/system/suites.md" "$work/inventory"
echo 'win-run-smoke: inventory passed'

mkdir -p "$work/tree's path/build" "$work/tree's path/tools/etude/win"
for file in hold.h etude_hold.c win/hold.c win/winfmt.c win/winfmt.h; do
    cp "$repo/tools/etude/$file" "$work/tree's path/tools/etude/$file"
done
cat > "$work/tree's path/check.sh" <<'SCRIPT'
set -eu
repo=$1
shift
. "$repo/tools/etude/hold.sh"
tree=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
etude_hold "$tree" win-run-smoke "$0" "$repo" "$@"
etude_held "$tree"
[ "$(sed -n 's/^pid //p' "$tree/build/.run.lock/owner")" = "$RACH2_TREE_HELD" ]
[ -z "${MSYS2_ARG_CONV_EXCL:-}" ]
printf '%s\000' "$@" > "$tree/actual"
exit 23
SCRIPT
# shellcheck disable=SC2016 # shell syntax must reach the script literally
set -- 'two words' "single'quote" 'double"quote' 'back\slash' "end\\" '' \
    /tmp/literal 'C:/native path' '$(echo wrong)' '*'
printf '%s\000' "$@" > "$work/expected"
status=0
sh "$work/tree's path/check.sh" "$repo" "$@" || status=$?
[ "$status" -eq 23 ]
if ! cmp "$work/expected" "$work/tree's path/actual"; then
    od -An -tc "$work/tree's path/actual" >&2
    exit 1
fi
[ ! -d "$work/tree's path/build/.run.lock" ]
finished=1
echo 'win-run-smoke: result: passed: native bootstrap, shell arguments and tree hold'
