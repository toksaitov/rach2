#!/bin/sh
# fdlock_mutate.sh [--direct] [FAULT...] | --texts: show that `tests/run
# fdlock` catches seven compiled faults in the locking of open descriptors,
# on the kernel, probes and emulator of the architecture ARCH names.
#
# `namespace` takes the namespace lock out of rach2/sys/fs/fd.c's operations
# on a file system whose namespace is serialised. `raw-read` and `raw-write`
# let a raw device's read or write run without the shared offset held;
# `lseek` and `llseek` take the descriptor lock out of a raw device's seeks;
# `readv` makes read_ready call the device's read without read_ops. `orphan`
# makes vfs.c truncate a krffs file by its path, which an unlinked file no
# longer has. Each fault must fail the suite with its row's words, and the
# suite must pass with the source restored. --direct runs `make run-fdlock`
# in place of `tests/run fdlock`. The default is every fault, without
# `llseek` on x86-64. --texts checks that each fault is once in its source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FAULT SOURCE: put FAULT into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

fault, name = sys.argv[1:]
path = Path(name)
text = path.read_text()
if fault == 'namespace':
    for verb in ('lock', 'unlock'):
        old = ('    if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {\n'
               f'        vfs_ns_{verb}(ofd->vmnt);\n    }}\n')
        assert text.count(old) == 1
        text = text.replace(old, '')
elif fault in ('raw-read', 'raw-write'):
    name = fault.removeprefix('raw-')
    start = text.index(f'static int32_t {name}_ops(')
    held = 'struct open_file *held = ops_offset_held(entry);'
    at = text.index(held, start)
    text = text[:at] + text[at:].replace(
        held, 'struct open_file *held = devfs_fd_is_raw(entry) ? NULL : ops_offset_held(entry);', 1)
elif fault in ('lseek', 'llseek'):
    if fault == 'lseek':
        start = text.index('static long raw_lseek(')
        end = text.index('\n}\n', start) + 3
    else:
        start = text.index('if (devfs_fd_is_raw(entry)) {', text.index('long sys__llseek('))
        end = text.index('if (devfs_fd_log_size(', start)
    branch = text[start:end]
    assert branch.count('fd_ofd_lock(entry->ofd);') == 1
    assert branch.count('fd_ofd_unlock(entry->ofd);') == 2
    branch = '\n'.join(line for line in branch.split('\n') if 'fd_ofd_' not in line)
    text = text[:start] + branch + text[end:]
elif fault == 'readv':
    start = text.index('static int32_t read_ready(')
    at = text.index('return read_ops(entry, buf, count);', start)
    text = text[:at] + text[at:].replace('return read_ops(entry, buf, count);',
                                       'return entry->ops->read(entry, buf, count);', 1)
else:
    old = '    rc = krffs_truncate_node(&n, (uint32_t)size);'
    assert text.count(old) == 1
    text = text.replace(old, '''    {
        char path[256];

        if (krffs_node_path(handle, path, sizeof(path)) != 0) {
            return -EIO;
        }
        rc = krffs_truncate(path + (path[0] == '/'), (uint32_t)size);
        n = krffs_resolve(path + (path[0] == '/'));
    }''')
path.write_text(text)
PY
}

# select_fault FAULT: the source FAULT changes, in `changed`, and the words
# its failing row prints, in `expected`.
select_fault()
{
    case "$1" in
        namespace) changed=rach2/sys/fs/fd.c
            expected='operation receives relocated handle' ;;
        raw-read) changed=rach2/sys/syscall_fs.c
            expected='concurrent raw reader consumes second sector' ;;
        raw-write) changed=rach2/sys/syscall_fs.c
            expected='raw shared offset retains both operations' ;;
        lseek) changed=rach2/sys/syscall_fs.c
            expected='raw lseek waits for transfer offset' ;;
        llseek) changed=rach2/sys/syscall_fs.c
            expected='raw llseek waits for transfer offset' ;;
        orphan) changed=rach2/sys/fs/vfs.c
            expected='shrink orphan (got -5)' ;;
        readv) changed=rach2/sys/syscall_fs.c
            expected='raw readv reserves its second sector' ;;
    esac
}

if [ "${1:-}" = --texts ]; then
    for fault in namespace raw-read raw-write lseek llseek orphan readv; do
        select_fault "$fault"
        RACH2_FAULT_TEXTS=1 apply_fault "$fault" "$changed" >/dev/null || exit 1
    done
    echo 'fdlock-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" fdlock-mutation "$repo/tests/system/fdlock_mutate.sh" "$@"

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
runner=suite
if [ "${1-}" = --direct ]; then
    runner=direct
    shift
fi
# x86-64 has no _llseek: its probe seeks through lseek, which the lseek fault
# covers, so the llseek fault is i686's alone.
if [ "$#" -eq 0 ]; then
    if [ -n "$ARCH_SUFFIX" ]; then
        set -- namespace raw-read raw-write lseek orphan readv
    else
        set -- namespace raw-read raw-write lseek llseek orphan readv
    fi
fi
for fault do
    case "$fault" in
        namespace|raw-read|raw-write|lseek|orphan|readv) ;;
        llseek)
            [ -z "$ARCH_SUFFIX" ] || {
                echo "fdlock-mutation: x86-64 has no _llseek for the llseek fault" >&2; exit 2;
            } ;;
        *) echo "fdlock-mutation: unknown fault: $fault" >&2; exit 2 ;;
    esac
done
mkdir -p build
work=$(mktemp -d "$repo/build/fdlock-proof.XXXXXX")
child=
changed=
for source in rach2/sys/fs/fd.c rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c; do
    cp "$source" "$work/$(basename "$source").saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"

# Stop the running boot's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        "$work/runutil" killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved copy of the changed source and touch it.
restore()
{
    if [ -n "$changed" ]; then
        cp "$work/$(basename "$changed").saved" "$changed"
        touch "$changed"
        changed=
    fi
}

# Stop the boot and, when a fault is in the source, restore it and rebuild.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        force_rebuild
        make -C rach2 "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1
        make build/tests/host/fd_hosttest "ARCH=$RACH2_ARCH" >>"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Remove the three changed objects, the kernel and fd_hosttest, so that make
# builds them again.
force_rebuild()
{
    rm -f "$tree"/rach2/sys/fs/fd.o "$tree"/rach2/sys/fs/vfs.o \
        "$tree"/rach2/sys/syscall_fs.o "$tree"/rach2/kernel build/tests/host/fd_hosttest
}

# run_phase PHASE EXPECTED: build the kernel, the probes and fd_hosttest and
# run the suite. With EXPECTED `pass` it must pass; otherwise it must fail
# and its logs must have EXPECTED.
run_phase()
{
    phase=$1
    expected=$2
    force_rebuild
    make -C rach2 "ARCH=$RACH2_ARCH" >"$work/$phase.build.log" 2>&1
    make -C tests "ARCH=$RACH2_ARCH" >>"$work/$phase.build.log" 2>&1
    make build/tests/host/fd_hosttest "ARCH=$RACH2_ARCH" >>"$work/$phase.build.log" 2>&1
    cksum "$tree"/rach2/kernel build/tests/host/fd_hosttest \
        "$tree"/tests/fdstale "$tree"/tests/bootprobe >"$work/$phase.artifacts"
    rm -f "$tree"/images/fdlock-test.disk.orphan.log "$tree"/images/fdlock-test.disk.raw.log
    if [ "$runner" = direct ]; then
        "$work/runutil" group make run-fdlock "ARCH=$RACH2_ARCH" >"$work/$phase.log" 2>&1 &
    else
        ARCH=$RACH2_ARCH "$work/runutil" group tests/run fdlock >"$work/$phase.log" 2>&1 &
    fi
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    for kind in orphan raw; do
        log="$tree"/images/fdlock-test.disk.$kind.log
        [ ! -f "$log" ] || cp "$log" "$work/$phase.$kind.log"
    done
    cat "$work/$phase.build.log" "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "fdlock-mutation: fault passed: $phase" >&2; exit 1;
        }
        grep -F "$expected" "$work/$phase.log" "$work/$phase."*.log >/dev/null || {
            echo "fdlock-mutation: wrong failure: $phase" >&2; exit 1;
        }
    fi
    echo "fdlock-mutation: result: $phase status $status"
}

echo "fdlock-mutation: logs: $work"
run_phase fixed pass
for fault do
    select_fault "$fault"
    apply_fault "$fault" "$changed"
    touch "$changed"
    run_phase "$fault" "$expected"
    restore
    run_phase "$fault-restored" pass
done
for source in rach2/sys/fs/fd.c rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c; do
    cmp "$source" "$work/$(basename "$source").saved"
done
echo "fdlock-mutation: pass: $# compiled faults rejected and restored"
