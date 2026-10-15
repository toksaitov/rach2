#!/bin/sh
# Prove the boot loader window's host tests against faults in private copies.
#
# The grubslot, partition and devfs tests each pass on a private copy of the
# kernel tree and of grubslot.c; each fault below goes into that copy alone,
# must fail the test that owns it with that row's words, and the copy restored
# must pass again and match the tree byte for byte.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR FAULT: put FAULT into its source in DIR, a tree laid out as
# sys/ and utils/; with RACH2_FAULT_TEXTS set, only check its text and write
# nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

work = pathlib.Path(sys.argv[1])
fault = sys.argv[2]
faults = {
    # The window's tails as the gap's constant, whatever the window is.
    "tails": ("utils/grubslot.c",
              "    tail = (uint32_t)((sectors - 1u) / 2u);\n",
              "    tail = GRUBSLOT_TAIL_SECTORS;\n"),
    # A candidate judged against the longest tail, not this window's.
    "bound": ("utils/grubslot.c",
              "    if (sectors - 1u > w->tail) {\n",
              "    if (sectors > GRUBSLOT_MAX_IMAGE_SECTORS) {\n"),
    # A running image judged against the longest tail, not this window's.
    "active": ("utils/grubslot.c",
               "    if (len == 0u || (uint32_t)len > w->tail) {\n",
               "    if (len == 0u || (uint32_t)len > GRUBSLOT_TAIL_SECTORS) {\n"),
    # The unread copy's array forgotten.
    "other-array": ("sys/block/partition.c",
                    "    if (ranges_meet(start, n, other_array, g->array_sectors)) {\n",
                    "    if (other_array == 1) {\n"),
    # The first BIOS boot partition taken, where a second makes it unclear.
    "one-entry": ("sys/block/partition.c",
                  "            if (found) {\n                return -1;\n            }\n",
                  ""),
    # The serving header's own array forgotten.
    "own-array": ("sys/block/partition.c",
                  "        ranges_meet(start, n, g->array_lba, g->array_sectors)) {\n",
                  "        0) {\n"),
    # A partition past the disk's end published all the same.
    "past-disk": ("sys/fs/devfs.c",
                  "    if (count > backing->sector_count || first > backing->sector_count - count) {\n",
                  "    if (0) {\n"),
}
name, before, after = faults[fault]
path = work / name
source = path.read_text()
if source.count(before) != 1:
    raise SystemExit("grubwindow-mutation: source pattern is not unique: " + fault)
path.write_text(source.replace(before, after))
PY
}

faults='tails bound active other-array one-entry own-array past-disk'
if [ "${1:-}" = --texts ]; then
    # The faults name their files as the private tree lays them out.
    root=$(mktemp -d "${TMPDIR:-/tmp}/rach2-grubwindow-texts.XXXXXX")
    trap 'rm -rf "$root"' EXIT
    ln -s "$repo/rach2/sys" "$root/sys"
    ln -s "$repo/utils" "$root/utils"
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$root" "$fault" >/dev/null || exit 1
    done
    echo 'grubwindow-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" grubwindow-mutation "$repo/tests/host/grubwindow_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/grubwindow-proof.XXXXXX")
child=
mkdir -p "$work/utils" "$work/saved"
cp -R rach2/sys "$work/sys"
cp utils/grubslot.c utils/grubslot.h utils/bootslot.h "$work/utils/"
cp utils/grubslot.c rach2/sys/block/partition.c rach2/sys/fs/devfs.c "$work/saved/"

# restore: copy the saved grubslot.c, partition.c and devfs.c back over the
# private copies.
restore()
{
    cp "$work/saved/grubslot.c" "$work/utils/grubslot.c"
    cp "$work/saved/partition.c" "$work/sys/block/partition.c"
    cp "$work/saved/devfs.c" "$work/sys/fs/devfs.c"
    touch "$work/utils/grubslot.c" "$work/sys/block/partition.c" "$work/sys/fs/devfs.c"
}

# cleanup: the exit trap; stop the running test and restore the sources.
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

# run_phase TEST PHASE EXPECTED MESSAGE: compile tests/host/TEST_hosttest.c
# against the private copies as the host suite compiles it, run it for at most
# 30 seconds, and require its status to be 0 if EXPECTED is 0 and nonzero
# otherwise, and its output to have MESSAGE.
run_phase()
{
    test=$1
    phase=$2
    expected=$3
    message=$4
    source=tests/host/${test}_hosttest.c
    binary="$work/$test-$phase"
    deps=$(grep -oE 'host-deps:[^*]*' "$source" | sed -e 's/host-deps://' \
        -e "s|rach2/sys/|$work/sys/|g" -e "s|utils/|$work/utils/|g" | tr '\n' ' ')
    # shellcheck disable=SC2086 # the dependencies are separate words
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wno-empty-body \
        -ffreestanding -I "$work/sys" -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 \
        -I "$work/sys/lib" -I "$work/sys/proc" -I "$work/utils" -I krffs \
        -I tests/host/compat -o "$binary" "$source" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 30; exec @ARGV or die "grubwindow-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$expected" -eq 0 ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || exit 1
    fi
    grep -Fq "$message" "$binary.log" || exit 1
    echo "grubwindow-mutation: result: $test $phase status $status"
}

# pass_of TEST: print the line TEST's host test prints when it passes.
pass_of()
{
    case $1 in
        grubslot) echo 'grubslot-hosttest: result: pass:' ;;
        partition) echo 'partition_hosttest: PASS' ;;
        devfs) echo 'devfs-hosttest: result: pass:' ;;
    esac
}

echo "grubwindow-mutation: logs: $work"
for test in grubslot partition devfs; do
    run_phase "$test" fixed 0 "$(pass_of "$test")"
done

for fault in $faults; do
    apply_fault "$work" "$fault"
    case $fault in
        tails) test=grubslot message='two tails of 1006 sectors' ;;
        bound) test=grubslot message='one of 1008 does not' ;;
        active) test=grubslot message="an image longer than the partition's tail" ;;
        other-array) test=partition message='though the primary never read that array' ;;
        one-entry) test=partition message='since which one is meant is not known' ;;
        own-array) test=partition message='an entry over the primary array is no window' ;;
        past-disk) test=devfs message='nor one whose partition runs past the disk' ;;
    esac
    run_phase "$test" "$fault-mutated" 1 "$message"
    restore
    run_phase "$test" "$fault-restored" 0 "$(pass_of "$test")"
    cmp "$work/utils/grubslot.c" utils/grubslot.c
    cmp "$work/sys/block/partition.c" rach2/sys/block/partition.c
    cmp "$work/sys/fs/devfs.c" rach2/sys/fs/devfs.c
done
echo "grubwindow-mutation: result: every fault failed its row and every restore passed"
