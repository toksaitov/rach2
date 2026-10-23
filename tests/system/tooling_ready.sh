#!/bin/sh
#
# tooling_ready.sh: make etude's tooling machine (tests/system/tooling.sh)
# for the host checks that run in it, as the host suite's preparation does
# through `make build/tests/host/tooling_machine`.
#
# When the machine of the tree's recipe is not made, `etude provision tooling
# prepare` makes it, which downloads the image and provisions it once. FILE then
# has the line `etude provision: result: made KEY DISK` of `etude provision
# tooling check`, and is written only when that line changes, so that the
# preparation's record of FILE stays the same. The exit status is 0, or 1 when
# the machine cannot be made.
#
# Usage: sh tests/system/tooling_ready.sh FILE
#
set -u
OUT=${1:?file}
REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd) || exit 1
# shellcheck source=tests/system/tooling.sh
. "$REPO/tests/system/tooling.sh"
[ -x "$TOOLING_BIN" ] || { echo "tooling-ready: $TOOLING_BIN is not built" >&2; exit 1; }
tooling_made || tooling prepare >&2 || exit 1
line=$(tooling check 2>/dev/null) || exit 1
[ "$(cat "$OUT" 2>/dev/null)" = "$line" ] && exit 0
printf '%s\n' "$line" > "$OUT.tmp" && mv "$OUT.tmp" "$OUT" || exit 1
