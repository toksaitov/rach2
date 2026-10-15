#!/bin/sh
# Prove fat32_hosttest's rows on what the two synced sequences leave.
#
# Usage: sh tests/host/fat_synced_mutate.sh [--texts]
#
# The unlink and the truncate of a file its fsync put on the medium are read
# back after the sequences run: the unlinked file must be gone
# and the truncated one must read its first 700 bytes. The test is compiled
# against a private copy of rach2/sys as it is, where it must pass; with
# fat_unlink leaving the name in its directory, where the first row must
# fail; with fat_truncate's shrink leaving the old size, where the second
# must fail; and restored, touched and compiled again, where it must pass.
# The volumes are built in a private fixture directory. The trap stops the
# test before it restores the source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" fat-synced-proof "$repo/tests/host/fat_synced_mutate.sh" "$@"
fi
if [ -n "$texts" ]; then
    work=
    source=rach2/sys/fs/fat32.c
else
    mkdir -p build/tests
    work=$(mktemp -d "$repo/build/tests/fat-synced-proof.XXXXXX")
    fixtures=$(mktemp -d /tmp/rach2-fix.XXXXXX)
    mkdir -p "$work/rach2"
    cp -R rach2/sys "$work/rach2/sys"
    source="$work/rach2/sys/fs/fat32.c"
    cp "$source" "$work/fat32.saved"
fi
child=

# stop_child: end the process group of the running test, if there is one, and
# wait until none of its processes is left.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# restore: copy the saved fat32.c back over the private tree.
restore()
{
    cp "$work/fat32.saved" "$source"
    touch "$source"
}

# cleanup: the exit trap; stop the running test, restore the source and remove
# the fixtures.
cleanup()
{
    stop_child
    restore
    rm -rf "$fixtures"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE EXPECTED DETAIL: compile the test against the private tree
# and run it for at most 900 seconds; a pass must exit 0 and print its OK
# line, a fail must print DETAIL as a failed row.
run_phase()
{
    phase=$1
    expected=$2
    detail=$3
    deps=$(grep -oE 'host-deps:[^*]*' tests/host/fat32_hosttest.c | sed 's/host-deps://' |
        tr '\n' ' ' | sed "s# rach2/# $work/rach2/#g")
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -DFIXDIR="\"$fixtures\"" \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$work/$phase.bin" tests/host/fat32_hosttest.c $deps </dev/null \
        >"$work/$phase.compile.log" 2>&1; then
        cat "$work/$phase.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$work/$phase.bin" >"$work/$phase.cksum"
    perl -e 'setpgrp(0, 0) or die "fat-synced-proof: setpgrp: $!\n";
             alarm 900; exec @ARGV or die "fat-synced-proof: exec: $!\n"' \
        "$work/$phase.bin" </dev/null >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    grep 'FAIL' "$work/$phase.log" || :
    if [ "$expected" = pass ]; then
        if [ "$status" -ne 0 ] || ! grep -q '^fat32_hosttest: OK' "$work/$phase.log"; then
            echo "fat-synced-proof: fail: $phase: status $status" >&2
            exit 1
        fi
    elif ! grep -F "FAIL: $detail" "$work/$phase.log" >/dev/null; then
        echo "fat-synced-proof: fail: $phase: the row did not fail: $detail" >&2
        exit 1
    fi
    echo "fat-synced-proof: result: $phase status $status"
}

# fault OLD NEW: replace the lines OLD of the private fat32.c, which must
# occur once, with NEW.
fault()
{
    python3 - "$source" "$1" "$2" <<'PYTHON'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
text = path.read_text()
old, new = sys.argv[2] + '\n', sys.argv[3] + '\n'
if text.count(old) != 1:
    raise SystemExit('fat-synced-proof: the lines to change are not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(old, new))
PYTHON
    [ -n "$texts" ] || touch "$source"
}

if [ -n "$texts" ]; then
    trap - EXIT
    run_phase() { :; }
    restore() { :; }
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "fat-synced-proof: logs: $work"
fi
run_phase fixed pass ''

fault '        rc = free_name_entries(fm, d->first, &sc);
    }
    if (rc != 0) {
        return rc;
    }
    return retire_chain(mnt, fm, dir_cluster, sc.index, entry_first(sc.ent.raw));' \
    '        rc = 0;
    }
    if (rc != 0) {
        return rc;
    }
    return retire_chain(mnt, fm, dir_cluster, sc.index, entry_first(sc.ent.raw));'
run_phase unlink-kept fail 'the file unlinked after a sync is gone'
restore
run_phase unlink-restored pass ''

fault '        n->size = (uint32_t)size;
        if (keep == 0) {' '        if (keep == 0) {'
run_phase size-kept fail 'the file truncated after a sync reads back its first 700 bytes'
restore
run_phase size-restored pass ''

if [ -n "$texts" ]; then
    echo 'fat-synced-proof: texts: every fault is once in its source'
    exit 0
fi
cmp "$work/fat32.saved" "$source"
echo 'fat-synced-proof: result: pass: each fault failed its row; the restored source passed'
