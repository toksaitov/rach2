#!/bin/sh
# proof.sh: run etude's proof driver on a catalogue with this tree held, so
# that no suite builds or boots the tree and no other proof saves a source
# while a fault is in it. The driver is built first, under the hold; --texts
# holds nothing and builds nothing.
#
# Usage: sh tools/etude/proof.sh [-w WORK] [-b STORE] [-k] PROOF [FAULT...]
#        sh tools/etude/proof.sh --texts PROOF...
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" etude-proof "$repo/tools/etude/proof.sh" "$@"
    make -s build/tools/etude/etude-proof >&2
fi
exec "$repo/build/tools/etude/etude-proof" -t "$repo" "$@"
