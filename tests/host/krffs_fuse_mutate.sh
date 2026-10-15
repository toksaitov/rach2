#!/bin/sh
# Prove krffs_fuse_hosttest.c with private mutations of the FUSE driver and
# the KRFFS library under krffs/, one at a time: the check of the node ring
# before FUSE starts taken out, a child found without its directory, a moved
# node that loses its directory, a rename that leaves the node in its old
# directory, and mkdir without its refusal of a taken directory id. The test
# is compiled with a fuse.h that stops the build, so that it uses the test's
# own stub.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

faults='ring parent moved rename mkdir-id'

# select_fault FAULT: the file FAULT changes, in `file`, the text it replaces,
# in `old`, the text that replaces it, in `new`, and the rows it must fail,
# in `rows`, one a line.
select_fault()
{
    case "$1" in
        ring)
            file=krffs/krffs.c
            old='if (!valid_node_ring(&file_system))'
            new='if (0)'
            rows='oversized root rejected before fuse
zero node size rejected before fuse' ;;
        parent)
            file=krffs/krffs_node.c
            old='node->type == KRFFS_Reserved_Node && node->parent_id == directory_id &&'
            new='node->type == KRFFS_Reserved_Node &&'
            rows='staged file of the same name at the root reads' ;;
        moved)
            file=krffs/krffs_allocator.c
            old='node->parent_id = original_node->parent_id;'
            new=''
            rows='moved file keeps its directory' ;;
        rename)
            file=krffs/krffs_fuse_operations.c
            old='node->parent_id = krffs_get_directory_id(KRFFS_File_System, new_directory);'
            new=''
            rows='rename across directories' ;;
        mkdir-id)
            file=krffs/krffs_fuse_operations.c
            old='directory_id_taken(directory, name)'
            new='0'
            rows='mkdir refuses a taken directory id' ;;
        *)
            echo "krffs-fuse-mutation: unknown fault: $1" >&2
            exit 1 ;;
    esac
}

# fault_once FILE: exit unless the text of the selected fault is once in
# FILE.
fault_once()
{
    if [ "$(grep -cF -- "$old" "$1" 2>/dev/null)" != 1 ]; then
        echo "krffs-fuse-mutation: the text of the fault is not once in $1: $old" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        select_fault "$fault"
        fault_once "$file"
    done
    echo 'krffs-fuse-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" krffs-fuse-mutation "$repo/tests/host/krffs_fuse_mutate.sh" "$@"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-fuse-proof.XXXXXX")
child=
source=
test=tests/host/krffs_fuse_hosttest.c
deps=$(sed -n 's/^.*host-deps: *//p' "$test" | tr '\n' ' ')
extra=$(sed -n 's/^.*host-cflags: *//p' "$test" | tr '\n' ' ')
mkdir -p "$work/krffs" "$work/tests/host/compat" "$work/no-fuse" "$work/fixtures"
cp "$test" "$work/tests/host/"
cp -R tests/host/fuse "$work/tests/host/"
cp tests/host/compat/fixdir_host.h "$work/tests/host/compat/"
cp krffs/krffs.c krffs/stageimage.krffs.c krffs/*.h "$work/krffs/"
for dep in $deps; do
    cp "$dep" "$work/$dep"
done
printf '#error "krffs-fuse-test: test stub was not selected"\n' > "$work/no-fuse/fuse.h"

# restore: stop the running test and put the saved copy of the faulted
# source back; the script calls it after each fault, and it is the exit trap.
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ -n "$source" ]; then
        cp "$work/saved" "$source"
        touch "$source"
        source=
    fi
}

trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE: compile the test over the copy as $work/PHASE and run it for
# at most 60 seconds. A phase named for a fault must fail each of its rows;
# any other must pass. Exit if not.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding -I krffs -I tests/host/compat \
        -DFIXDIR="\"$work/fixtures\"" $extra -I "$work/no-fuse" -MMD -MF "$binary.d" \
        -o "$binary" "$test" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum krffs/*.c "$binary" > "$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "krffs-fuse-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    case " $faults " in
        *" $phase "*)
            [ "$status" -eq 1 ] || exit 1
            printf '%s\n' "$rows" | while IFS= read -r row; do
                grep -qF "krffs-fuse-test: fail: $row" "$binary.log" || exit 1
            done || exit 1 ;;
        *)
            [ "$status" -eq 0 ] || exit 1 ;;
    esac
    echo "krffs-fuse-mutation: result: $phase status $status"
}

echo "krffs-fuse-mutation: logs: $work"
cd "$work"
run_phase fixed
for fault in $faults; do
    select_fault "$fault"
    fault_once "$file"
    cp "$file" "$work/saved"
    source="$work/$file"
    OLD=$old NEW=$new perl -0pi -e 's/\Q$ENV{OLD}\E/$ENV{NEW}/' "$source"
    touch "$source"
    run_phase "$fault"
    restore
    cmp "$work/saved" "$file"
done
run_phase restored
