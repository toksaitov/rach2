#!/bin/sh
# legacy.sh: prepare suites of tests/registry with etude's preparation
# adapter, check a preparation's record, or run a prepared suite with the
# suite adapter, with this tree held so that no run builds or boots it
# meanwhile. The adapter is built first, under the hold. ARCH and
# RACH2_KERNEL choose the architecture and the kernel of a preparation, as
# they do for tests/run; a run takes both from its record.
#
# Usage: sh tools/etude/legacy.sh prepare [-w WORK] [-o RECORD] [-s SECONDS]
#                                    SUITE...
#        sh tools/etude/legacy.sh check [-d] RECORD
#        sh tools/etude/legacy.sh run [-w WORK] [-i IDLE] [-s SECONDS] RECORD
#                                    SUITE
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-legacy "$repo/tools/etude/legacy.sh" "$@"
make -s build/tools/etude/etude-legacy >&2
if [ "${1:-}" = prepare ]; then
    shift
    set -- prepare -t "$repo" -a "${ARCH:-i686}" ${RACH2_KERNEL:+-n "$RACH2_KERNEL"} "$@"
fi
exec "$repo/build/tools/etude/etude-legacy" "$@"
