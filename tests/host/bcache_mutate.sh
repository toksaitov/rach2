#!/bin/sh
# Prove the rows of tests/host/bcache_cost_hosttest.c against the faults they
# were written for, entries 25 and 40 of milestone 50: the hold rules walking
# every buffer of the cache on every get, the victim chosen by a walk of every
# buffer, a sync walking the cache again for every run it writes, and the
# cache's cap at half the kernel heap's pages taken out; against a sync's
# batch kept by insertion sort, which shifted the whole batch for each block
# of a file written in order, and a sync that gathers 128 blocks a walk with
# room for all of them; against the cap held at half of i686's region,
# as it was before the boot sized the region from the memory on x86-64; and
# against a visit counter that answers 0, which only the rows' lower bounds
# can see. Each fault is compiled into a private copy of
# rach2/sys/fs/bcache.c, one at a time, and must fail each row its case names;
# the copy restored, touched and rebuilt must pass.
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
# phase must exit 0 with MESSAGE in its output; a failing one must exit
# nonzero, not by a signal, with a failure for each line of MESSAGE.
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
        [ "$status" -gt 0 ] && [ "$status" -lt 128 ] || exit 1
        while IFS= read -r row; do
            grep -Fq "FAIL: $row" "$binary.log" || exit 1
        done <<EOF
$message
EOF
    fi
    echo "bcache-proof: result: $phase status $status"
}

echo "bcache-proof: logs: $work"
run_phase fixed pass 'bcache_cost_hosttest: result: pass:'
for fault in hold-walk victim-walk sync-rescan insertion-sort small-batch cap fixed-cap uncounted; do
    python3 - "$source" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
fault = sys.argv[2]
text = path.read_text()
# Each fault but the last puts back the code as it stood before its fix, in
# the one function it lived in; the sync's is its batch cut to one block,
# which walks the cache once for every run written, as the sync did before it
# gathered batches; the insertion sort counts each shift as the heap counts
# each step; and the small batch is the stack's 128 whatever room the heap
# has. The counting line in each walk stays where it is. The last leaves
# every walk as it is and makes the counter answer 0.
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
    'sync-rescan': (None, [
        ('#define SYNC_BATCH       128\n', '#define SYNC_BATCH       1\n'),
        ('batch, (int)(pages * (PAGE_SIZE / 8u)));\n', 'batch, 1);\n')]),
    'small-batch': ('static int sync_pass(block_device_t *dev, int pass, uint32_t flags, '
                    'uint32_t first_flags)\n{', [
        ('    if (ndirty > SYNC_BATCH) {\n', '    if (0) {\n')]),
    'insertion-sort': ('static int gather_dirty(block_device_t *dev, int pass, int started, '
                       'uint64_t after, uint64_t *out,\n                        int cap)\n{', [
        ('        if (n < cap) {\n'
         '            out[n] = b->blk;\n'
         '            batch_sift_up(out, n);\n'
         '            n++;\n'
         '        } else if (b->blk < out[0]) {\n'
         '            out[0] = b->blk;\n'
         '            batch_sift_down(out, n, 0);\n'
         '        }\n'
         '    }\n'
         '    for (i = n - 1; i > 0; i--) {\n'
         '        uint64_t highest = out[0];\n'
         '\n'
         '        out[0] = out[i];\n'
         '        out[i] = highest;\n'
         '        batch_sift_down(out, i, 0);\n'
         '    }\n',
         '        if (n == cap) {\n'
         '            if (b->blk >= out[n - 1]) {\n'
         '                continue;\n'
         '            }\n'
         '            n--;\n'
         '        }\n'
         '        for (i = n; i > 0 && out[i - 1] > b->blk; i--) {\n'
         '            stat_visits++;\n'
         '            out[i] = out[i - 1];\n'
         '        }\n'
         '        out[i] = b->blk;\n'
         '        n++;\n'
         '    }\n')]),
    'cap': ('static uint32_t bcache_limit(void)\n{', [
        ('    uint32_t n = share > most ? most : (uint32_t)share;\n',
         '    uint32_t n = (uint32_t)share;\n')]),
    'fixed-cap': ('static uint32_t bcache_limit(void)\n{', [
        ('    uint32_t most = kheap_region_pages() / 2u;\n',
         '    uint32_t most = KHEAP_FLOOR / PAGE_SIZE / 2u;\n')]),
    'uncounted': ('uint64_t bcache_stat_visits(void)\n{', [
        ('    return stat_visits;', '    return 0;')]),
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
        sync-rescan | insertion-sort)
            message="a sync gathers its dirty blocks in one walk and sorts them in a heap
without room for one batch, a sync reads the cache once for each 128 dirty blocks, not once for each run, and sorts each batch in a heap" ;;
        small-batch) message='a sync gathers its dirty blocks in one walk and sorts them in a heap' ;;
        cap) message="a machine whose memory share passes the kernel heap has a cache of half the heap's pages
a heap of another size has a cache of half its own pages" ;;
        fixed-cap) message='a heap of another size has a cache of half its own pages' ;;
        uncounted) message="the counter counts the buffers a hit and its release read
the counter counts the buffers each miss and its release read
the counter counts the sync's four walks of the cache
the counter counts the sync's eleven walks of the cache" ;;
    esac
    run_phase "$fault-mutated" fail "$message"
    restore
    run_phase "$fault-restored" pass 'bcache_cost_hosttest: result: pass:'
    cmp "$work/bcache.saved" "$source"
done
echo 'bcache-proof: result: pass: every fault failed its row and every restored copy passed'
