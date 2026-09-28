#!/bin/sh
# Prove the descriptor regressions with compiled faults and restored builds.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
runner=suite
if [ "${1-}" = --direct ]; then
    runner=direct
    shift
fi
if [ "$#" -eq 0 ]; then
    set -- namespace raw-read raw-write lseek llseek orphan readv
fi
for fault do
    case "$fault" in
        namespace|raw-read|raw-write|lseek|llseek|orphan|readv) ;;
        *) echo "fdlock-mutation: unknown fault: $fault" >&2; exit 2 ;;
    esac
done
[ ! -d build/.run.lock ] || {
    echo "fdlock-mutation: another suite owns this tree" >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/fdlock-proof.XXXXXX")
child=
changed=
for source in rach2/sys/fs/fd.c rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c; do
    cp "$source" "$work/$(basename "$source").saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"

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

restore()
{
    if [ -n "$changed" ]; then
        cp "$work/$(basename "$changed").saved" "$changed"
        touch "$changed"
        changed=
    fi
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        force_rebuild
        make -C rach2 >"$work/cleanup.build.log" 2>&1
        make build/tests/host/fd_hosttest >>"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

force_rebuild()
{
    rm -f build/rach2/sys/fs/fd.o build/rach2/sys/fs/vfs.o \
        build/rach2/sys/syscall_fs.o build/rach2/kernel build/tests/host/fd_hosttest
}

run_phase()
{
    phase=$1
    expected=$2
    force_rebuild
    make -C rach2 >"$work/$phase.build.log" 2>&1
    make -C tests >>"$work/$phase.build.log" 2>&1
    make build/tests/host/fd_hosttest >>"$work/$phase.build.log" 2>&1
    cksum build/rach2/kernel build/tests/host/fd_hosttest \
        build/tests/fdstale build/tests/bootprobe >"$work/$phase.artifacts"
    rm -f build/images/fdlock-test.disk.orphan.log build/images/fdlock-test.disk.raw.log
    if [ "$runner" = direct ]; then
        "$work/runutil" group make run-fdlock >"$work/$phase.log" 2>&1 &
    else
        "$work/runutil" group tests/run fdlock >"$work/$phase.log" 2>&1 &
    fi
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    for kind in orphan raw; do
        log=build/images/fdlock-test.disk.$kind.log
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
    case "$fault" in
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
    python3 - "$fault" "$changed" <<'PY'
from pathlib import Path
import sys

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
        start = text.index('static int32_t raw_lseek(')
        end = text.index('\n}\n', start) + 3
    else:
        start = text.index('if (devfs_fd_is_raw(entry)) {', text.index('int32_t sys__llseek('))
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
    touch "$changed"
    run_phase "$fault" "$expected"
    restore
    run_phase "$fault-restored" pass
done
for source in rach2/sys/fs/fd.c rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c; do
    cmp "$source" "$work/$(basename "$source").saved"
done
echo "fdlock-mutation: pass: $# compiled faults rejected and restored"
