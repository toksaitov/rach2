#!/bin/sh
# Compile the core file's writer before, during and after each fault: its
# descriptor left open to the processes that share the table, and the zero
# at the end of a file that ends in a hole written past the limit.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
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
changes = {
    "descriptor-open": ("    entry = &current_proc->fds->entries[fd];\n"
                        "    entry->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;\n"
                        "    if (!entry->ofd || !entry->ofd->vmnt) {",
                        "    entry = fd_get(current_proc->fds, fd);\n"
                        "    if (!entry || !entry->ofd || !entry->ofd->vmnt) {"),
    "zero-past-limit": ("rc = cf.written < most ? core_file_put(&cf, &zero, 1) : -EFBIG;",
                        "rc = core_file_put(&cf, &zero, 1);"),
}
old, new = changes[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit("corefile-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='descriptor-open zero-past-limit'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/syscall_fs.c "$fault" >/dev/null || exit 1
    done
    echo 'corefile-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" corefile-mutation "$repo/tests/host/corefile_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/corefile-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp rach2/kernel.h "$work/kernel.h"
mkdir "$work/vendor"
cp rach2/vendor/multiboot.h "$work/vendor/multiboot.h"
cp "$work/sys/syscall_fs.c" "$work/syscall_fs.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/syscall_fs.saved" "$work/sys/syscall_fs.c"
    touch "$work/sys/syscall_fs.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/arch/x86" -I "$work/sys/arch/i686" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/corefile_hosttest.c "$work/sys/fs/fd.c" \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/syscall_fs.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "corefile-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "corefile-mutation: result: $phase status $status"
}

echo "corefile-mutation: logs: $work"
run_phase fixed 0 'corefile-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$work/sys/syscall_fs.c" "$fault"
    case $fault in
        descriptor-open) message="a write through the core file's descriptor by another process finds none" ;;
        zero-past-limit) message='a limit of the bytes before the zero at the end answers EFBIG' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'corefile-hosttest: result: pass:'
    cmp "$work/syscall_fs.saved" "$work/sys/syscall_fs.c"
done
