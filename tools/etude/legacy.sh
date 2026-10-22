#!/bin/sh
# legacy.sh: build and record goals of this tree with etude's preparation
# adapter, or check a preparation's record, with this tree held so that no
# run builds or boots it meanwhile. The adapter is built first, under the
# hold. ARCH and RACH2_KERNEL choose the architecture and the kernel of a
# preparation, as they do for tests/run.
#
# Usage: sh tools/etude/legacy.sh prepare [-w WORK] [-o RECORD] [-s SECONDS]
#                                    -b GOAL [-b GOAL]...
#        sh tools/etude/legacy.sh check [-d] RECORD
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tools/etude/hold.sh"
etude_hold "$repo" etude-legacy "$repo/tools/etude/legacy.sh" "$@"
make -s build/tools/etude/etude-legacy >&2
if [ "${1:-}" = prepare ]; then
    shift
    set -- prepare -t "$repo" -a "${ARCH:-i686}" ${RACH2_KERNEL:+-n "$RACH2_KERNEL"} "$@"
fi
exec "$repo/build/tools/etude/etude-legacy" "$@"
