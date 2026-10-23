#!/bin/sh
# held.sh: run `etude prepare`, which builds and records goals of this tree,
# or `etude check record`, which checks a preparation's record, with this
# tree held so that no run builds or boots it meanwhile. etude is built
# first, under the hold. ARCH and RACH2_KERNEL choose the architecture and
# the kernel of a preparation, as they do for tests/run.
#
# Usage: sh tools/etude/held.sh prepare [-w WORK] [-o RECORD] [-s SECONDS]
#                                  -b GOAL [-b GOAL]...
#        sh tools/etude/held.sh check record [-d] RECORD
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tools/etude/hold.sh"
etude_hold "$repo" etude "$repo/tools/etude/held.sh" "$@"
make -s build/tools/etude/etude >&2
if [ "${1:-}" = prepare ]; then
    shift
    set -- prepare -t "$repo" -a "${ARCH:-i686}" ${RACH2_KERNEL:+-n "$RACH2_KERNEL"} "$@"
fi
exec "$repo/build/tools/etude/etude" "$@"
