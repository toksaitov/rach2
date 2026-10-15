#!/bin/sh
# Prove bootslot_hosttest.c with private mutations of utils/bootslot.c, the
# boot partition's slots. The test includes bootslot.c, so the private copy
# comes first on the include path.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of bootslot.c; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
source = path.read_text()
if sys.argv[2] == "order":
    start = source.index('    if (file_write_sector(bs, &bs->fallback, 0, head)')
    end = source.index('    return 0;', start)
    final = source[start:end]
    source = source[:start] + source[end:]
    before = '    for (s = 1; s < nsectors; s++) {\n'
    after = final + before
elif sys.argv[2] == "policy":
    before = '''    if (ro && !rw) {
        return fail(err, errlen,
                    "the image is the read-only disk build, which cannot mount a disk root");
    }
'''
    after = ''
elif sys.argv[2] == "bound":
    before = '''        if (p_memsz > 0xFFFFFFFFu - p_vaddr) {
            return fail(err, errlen, "a load leaves the 32-bit address space");
        }
'''
    after = ''
else:
    before, after = {
        "mb2": ('return fail(err, errlen, "no multiboot2 header in the first 8192 bytes");',
                'return policy_judged(image, present, size, err, errlen);'),
        "padded": ('return fail(err, errlen, "the load runs to the end of the file, which the slot pads");',
                   'load_end = load_addr + (size - file_start);'),
        "search": ('search = present < MULTIBOOT_SEARCH ? present : MULTIBOOT_SEARCH;',
                   'search = present;'),
        "chain": ('return len == size / cluster_bytes + (size % cluster_bytes != 0 ? 1u : 0u);',
                  'return size <= len * cluster_bytes && len * cluster_bytes - size < cluster_bytes;'),
        "tags": ('} else if (type != 4 && type != 5 && (le16(h + at + 2) & 1u) == 0) {',
                 '} else if (0) {'),
        "tagend": ('if (at > len || len - at < 8) {', 'if (len - at < 8) {'),
        "x86tags": ('if (x86_64 && address == 0) {', 'if (x86_64 < 0 && address == 0) {'),
        "pair": ('if ((address != 0) != (entry != 0)) {', 'if (0) {'),
    }[sys.argv[2]]
if source.count(before) != 1:
    raise SystemExit("bootslot-mutation: source pattern is not unique")
path.write_text(source.replace(before, after))
PY
}

faults='order bound policy mb2 padded search chain tags tagend x86tags pair'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault utils/bootslot.c "$fault" >/dev/null || exit 1
    done
    echo 'bootslot-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" bootslot-proof "$repo/tests/host/bootslot_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/bootslot-proof.XXXXXX")
child=
cp utils/bootslot.c "$work/bootslot.saved"
cp "$work/bootslot.saved" "$work/bootslot.c"

# cleanup: stop the running test and put the saved bootslot.c back; the loop
# calls it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/bootslot.saved" "$work/bootslot.c"
    touch "$work/bootslot.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile bootslot_hosttest.c over the copy as
# $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all -I "$work" \
        -I utils tests/host/bootslot_hosttest.c -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum tests/host/bootslot_hosttest.c utils/bootslot.h \
        "$work/bootslot.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "bootslot-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "bootslot-mutation: result: $phase status $status"
}

echo "bootslot-mutation: logs: $work"
run_phase fixed 0 'bootslot-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$work/bootslot.c" "$fault"
    case $fault in
        order) message='install refuses the unverified fallback' ;;
        bound) message='load crossing 4 GiB is refused' ;;
        policy) message="the read-only build is refused as a machine's kernel" ;;
        mb2) message='an i386 kernel without a multiboot2 header is refused' ;;
        padded) message='which the slot pads, is refused' ;;
        search) message='ends past the 8192 bytes the slot checks read is refused' ;;
        chain) message='a chain of 4 GiB and 64 KiB does not fit a file of 64 KiB' ;;
        tags) message='the check does not know is refused' ;;
        tagend) message="a multiboot2 tag padded past the header's end is refused" ;;
        x86tags) message='without its address and entry tags is refused' ;;
        pair) message='a multiboot2 address tag without an entry tag is refused' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'bootslot-hosttest: result: pass:'
    cmp "$work/bootslot.saved" "$work/bootslot.c"
done
