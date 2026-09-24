#!/bin/sh
# Prove the rows of tests/host/bcache_cost_hosttest.c against the faults they
# were written for, entries 25 and 40 of milestone 50: the hold rules walking
# every buffer of the cache on every get, the victim chosen by a walk of every
# buffer, a sync walking the cache again for every run it writes, and the
# cache's cap at half the kernel heap's pages taken out. Each fault is
# compiled into a private copy of rach2/sys/fs/bcache.c, one at a time, and
# must fail its own row; the copy restored, touched and rebuilt must pass.
# The test is compiled as tests/system/suites/host.sh compiles it, in a
# private tree laid out as the repository is, so its host-deps lines and its
# relative includes resolve to the private copy.
#
# Usage: bcache_mutate.sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || {
    echo 'usage: bcache_mutate.sh' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/bcache-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp -R tests/host/compat "$work/tests/host/compat"
cp tests/host/bcache_cost_hosttest.c "$work/tests/host/bcache_cost_hosttest.c"
source="$work/rach2/sys/fs/bcache.c"
cp "$source" "$work/bcache.saved"

restore()
{
    cp "$work/bcache.saved" "$source"
    touch "$source"
}
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

test=tests/host/bcache_cost_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$work/$test" | sed 's/host-deps://' | tr '\n' ' ')

# run_phase PHASE pass|fail MESSAGE: compile and run the test. A passing
# phase must exit 0 with MESSAGE in its output; a failing one must exit with
# its count of failures, not a signal, with MESSAGE among them.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    # shellcheck disable=SC2086 # the dependency list is one path a word
    if ! (cd "$work" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        -I tests/host/compat -o "$binary" "$test" $deps) >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 180; exec @ARGV or die "bcache-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] && grep -Fq "$message" "$binary.log" || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 128 ] &&
            grep -Fq "FAIL: $message" "$binary.log" || exit 1
    fi
    echo "bcache-proof: result: $phase status $status"
}

echo "bcache-proof: logs: $work"
run_phase fixed pass 'bcache_cost_hosttest: result: pass:'
for fault in hold-walk victim-walk sync-rescan cap; do
    python3 - "$source" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
fault = sys.argv[2]
text = path.read_text()
# Each fault puts back the code as it stood before its fix, in the one
# function it lived in; the sync's is its batch cut to one block, which
# walks the cache once for every run written, as the sync did before it
# gathered batches. The counting line in each walk stays where it is.
faults = {
    'hold-walk': ('static void check_hold_rules(block_device_t *dev, uint64_t blk)\n{', [
        ('    for (b = *held_list(); b; b = b->hold_next) {\n'
         '        stat_visits++;\n'
         '        if (!b->dev) {\n',
         '    for (b = bufs; b; b = b->next) {\n'
         '        stat_visits++;\n'
         '        if (b->holder != caller_pid() || !b->dev) {\n')]),
    'victim-walk': ('static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank, '
                    'int *fresh)\n{', [
        ('            for (b = lru_head; b; b = b->lru_next) {\n'
         '                stat_visits++;\n'
         '                if (b->holder != -1 || b == fill_keep) {\n',
         '            for (b = bufs; b; b = b->next) {\n'
         '                stat_visits++;\n'
         '                if (b->holder != -1 || b == fill_keep || !b->dev) {\n'),
        ('                victim = b;\n                break;\n',
         '                if (!victim || (int32_t)(b->stamp - victim->stamp) < 0) {\n'
         '                    victim = b;\n                }\n')]),
    'sync-rescan': (None, [('#define SYNC_BATCH 128\n', '#define SYNC_BATCH 1\n')]),
    'cap': ('static uint32_t bcache_limit(void)\n{', [
        ('    if (n > BCACHE_HEAP_SHARE) {\n        n = BCACHE_HEAP_SHARE;\n    }\n', '')]),
}
head, changes = faults[fault]
if head is None:
    start, end = 0, len(text)
else:
    if text.count(head) != 1:
        raise SystemExit('bcache-proof: the function for ' + fault + ' is not unique')
    start = text.index(head)
    end = text.index('\n}\n', start)
body = text[start:end]
for old, new in changes:
    if body.count(old) != 1:
        raise SystemExit('bcache-proof: the pattern for ' + fault + ' is not unique')
    body = body.replace(old, new)
path.write_text(text[:start] + body + text[end:])
PY
    if cmp -s "$work/bcache.saved" "$source"; then
        echo "bcache-proof: the $fault mutation did not change the source" >&2
        exit 1
    fi
    case $fault in
        hold-walk) message='a hit with three buffers held reads its holds and its own buffer, not the cache' ;;
        victim-walk) message='a miss on a full cache reads the head of the release order, not the cache' ;;
        sync-rescan) message='a sync reads the cache once for each 128 dirty blocks, not once for each run' ;;
        cap) message="a machine whose memory share passes the kernel heap has a cache of half the heap's pages" ;;
    esac
    run_phase "$fault-mutated" fail "$message"
    restore
    run_phase "$fault-restored" pass 'bcache_cost_hosttest: result: pass:'
    cmp "$work/bcache.saved" "$source"
done
echo 'bcache-proof: result: pass: every fault failed its row and every restored copy passed'
