#!/bin/sh
# Compile the FUSE test and its ring-validation mutation in a private tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# ring_once FILE: the check the fault removes is once in FILE.
ring_once()
{
    if [ "$(grep -c 'if (!valid_node_ring(&file_system))' "$1")" -ne 1 ]; then
        echo "krffs-fuse-mutation: the node ring check is not once in $1" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    ring_once krffs/krffs.c
    echo 'krffs-fuse-mutation: texts: every fault is once in its source'
    exit 0
fi
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-fuse-proof.XXXXXX")
child=
test=tests/host/krffs_fuse_hosttest.c
deps=$(sed -n 's/^.*host-deps: *//p' "$test" | tr '\n' ' ')
extra=$(sed -n 's/^.*host-cflags: *//p' "$test" | tr '\n' ' ')
mkdir -p "$work/krffs" "$work/tests/host" "$work/no-fuse"
cp "$test" "$work/tests/host/"
cp -R tests/host/fuse "$work/tests/host/"
# shellcheck disable=SC2086
cp krffs/krffs.c krffs/*.h $deps "$work/krffs/"
source="$work/krffs/krffs.c"
cp "$source" "$work/krffs.saved"
printf '#error "krffs-fuse-test: test stub was not selected"\n' > "$work/no-fuse/fuse.h"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/krffs.saved" "$source"
    touch "$source"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding -I krffs \
        $extra -I "$work/no-fuse" -MMD -MF "$binary.d" \
        -o "$binary" "$test" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" > "$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "krffs-fuse-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'krffs-fuse-test: fail: oversized root rejected before fuse' \
                "$binary.log" &&
            grep -q 'krffs-fuse-test: fail: zero node size rejected before fuse' \
                "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "krffs-fuse-mutation: result: $phase status $status"
}

echo "krffs-fuse-mutation: logs: $work"
cd "$work"
run_phase fixed
ring_once "$source"
sed 's/if (!valid_node_ring(\&file_system))/if (0)/' "$work/krffs.saved" > "$source"
touch "$source"
run_phase mutated
cleanup
run_phase restored
cmp "$work/krffs.saved" "$source"
