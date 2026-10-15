#!/bin/sh
# Prove fd_hosttest.c with private mutations of rach2/sys/fs/fd.c and
# proc/process.c. The `unchecked` fault ignores a failed allocation of a
# standard descriptor's description, `rollback` leaves the ones allocated
# before it in the table, and `process` publishes a process whose standard
# descriptors could not be made. The `repoint` fault moves the handle of a
# description on any mounted type when a node moves, not KRFFS's alone. The
# two namespace faults change the test that `fd_ofd_lock` and `fd_ofd_unlock`
# both make: one takes the mount's namespace for KRFFS alone, the other for
# every mounted file system whether or not its type asks to be serialised.
# The six allocation faults answer -1 where a refusal of a descriptor
# answers EMFILE, ENOMEM or EBADF. The two faults of `fd_ofd_live` call a
# description open for a later serial than its own, or while its last
# reference is closed. The split-modes fault opens fd 0 read-only and fds 1
# and 2 write-only, where each is open for reading and writing.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of fd.c or, for
# `process`, of process.c; with RACH2_FAULT_TEXTS set, only check its text
# and write nothing.
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
    "namespace": ("if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {",
                  "if (ofd->vmnt && ofd->vmnt->type == &vfs_krffs_type) {"),
    "unserialised": ("if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {",
                     "if (ofd->vmnt) {"),
    "repoint": ("mnt->type == &vfs_krffs_type && rec->ofd.fhandle == from",
                "rec->ofd.fhandle == from"),
    "process": ("if (fd_init_stdio(p->fds) != 0) {",
                "if (fd_init_stdio(p->fds) != 0 && 0) {"),
    "unchecked": ("        if (!ofd) {\n            while (i > FD_STDIN)",
                  "        if (0) {\n            while (i > FD_STDIN)"),
    "rollback": ("                fd_free(table, --i);", "                --i;"),
    "alloc-description": ("        if (!ofd) {\n            return -ENOMEM;",
                          "        if (!ofd) {\n            return -1;"),
    "alloc-growth": ("        if (rc != 0) {\n            return rc;",
                     "        if (rc != 0) {\n            return -1;"),
    "alloc-minimum": ("    if (!table || minfd < 0 || minfd >= limit) {\n        return -EMFILE;",
                      "    if (!table || minfd < 0 || minfd >= limit) {\n        return -1;"),
    "alloc-limit": ("    if (i >= limit) {\n        return -EMFILE;",
                    "    if (i >= limit) {\n        return -1;"),
    "reach-growth": ("    return fd_table_grow(table, fd + 1);",
                     "    return fd_table_grow(table, fd + 1) ? -1 : 0;"),
    "reach-range": ("    if (!table || fd < 0 || fd >= fd_limit(table)) {\n        return -EBADF;",
                    "    if (!table || fd < 0 || fd >= fd_limit(table)) {\n        return -1;"),
    "live-serial": ("return rec->ofd.refcount > 0 && rec->ofd.serial == serial;",
                    "return rec->ofd.refcount > 0 && rec->ofd.serial <= serial;"),
    "live-closing": ("return rec->ofd.refcount > 0 && rec->ofd.serial == serial;",
                     "return rec->ofd.serial == serial;"),
    "split-modes": ("table->entries[i].flags = FD_FLAG_READ | FD_FLAG_WRITE;",
                    "table->entries[i].flags = (i == FD_STDIN) ? FD_FLAG_READ : FD_FLAG_WRITE;"),
}
old, new = changes[sys.argv[2]]
if sys.argv[2] in ("namespace", "unserialised"):
    for name in ("fd_ofd_lock", "fd_ofd_unlock"):
        start = source.index("void " + name + "(")
        end = source.index("\n}", start) + 2
        body = source[start:end]
        if body.count(old) != 1:
            raise SystemExit("fd-stdio-mutation: lock pattern is not unique")
        source = source[:start] + body.replace(old, new) + source[end:]
else:
    if source.count(old) != 1:
        raise SystemExit("fd-stdio-mutation: source pattern is not unique")
    source = source.replace(old, new)
path.write_text(source)
PY
}

faults='unchecked rollback process repoint namespace unserialised
    alloc-description alloc-growth alloc-minimum alloc-limit reach-growth
    reach-range live-serial live-closing split-modes'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        source=rach2/sys/fs/fd.c
        [ "$fault" != process ] || source=rach2/sys/proc/process.c
        RACH2_FAULT_TEXTS=1 apply_fault "$source" "$fault" >/dev/null || exit 1
    done
    echo 'fd-stdio-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" fd-stdio-mutation "$repo/tests/host/fd_stdio_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/fd-stdio-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/fs/fd.c" "$work/fd-stdio.saved"
cp "$work/sys/proc/process.c" "$work/process.saved"

# cleanup: stop the running test and put the saved fd.c and process.c back;
# the loop calls it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/fd-stdio.saved" "$work/sys/fs/fd.c"
    cp "$work/process.saved" "$work/sys/proc/process.c"
    touch "$work/sys/fs/fd.c" "$work/sys/proc/process.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile fd_hosttest.c over the copy as
# $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/fd_hosttest.c "$work/sys/fs/fd.c" \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/fd.c" "$work/sys/proc/process.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "fd-stdio-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "fd-stdio-mutation: result: $phase status $status"
}

echo "fd-stdio-mutation: logs: $work"
run_phase fixed 0 'fd_hosttest: pass:'
for fault in $faults; do
    source="$work/sys/fs/fd.c"
    [ "$fault" != process ] || source="$work/sys/proc/process.c"
    apply_fault "$source" "$fault"
    case $fault in
        unchecked) message='stdio reports allocation failure' ;;
        process) message='failed process is not published' ;;
        rollback) message='stdio releases partial allocations' ;;
        repoint) message='mounted open handle remains stable during node relocation' ;;
        namespace) message='other mounted types take the namespace before the description' ;;
        unserialised) message='a file system that needs no serialising takes no namespace lock' ;;
        alloc-description) message='a description the heap has no room for is ENOMEM' ;;
        alloc-growth) message='a table the heap has no room to grow is ENOMEM' ;;
        alloc-minimum) message='a minimum at the limit is EMFILE' ;;
        alloc-limit) message='the process with a limit of 4 is held to it in the shared table' ;;
        reach-growth) message='a dup2 past the end of a table the heap cannot grow is ENOMEM' ;;
        reach-range) message='a dup2 destination outside the limit is EBADF' ;;
        live-serial) message='a description is not one with another serial' ;;
        live-closing) message='a description its file system closes after its last reference is not open' ;;
        split-modes) message='each stdio descriptor is open for reading and writing' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'fd_hosttest: pass:'
    cmp "$work/fd-stdio.saved" "$work/sys/fs/fd.c"
done
