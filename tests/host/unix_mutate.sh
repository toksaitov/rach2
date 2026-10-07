#!/bin/sh
# Prove each row of tests/host/unix_hosttest.c with a fault that takes its
# fix back in a private copy of rach2/sys: the descriptors accept and
# socketpair make published before their numbers are copied, and closed
# through the table when the copy fails, as they were; the slot a receive
# of SCM_RIGHTS reserves given its type before the copy; pipe2's ends given
# before the copy; connect's check of the socket's state after its sleeps
# removed; the bind lock and the receive lock never waited for, or let go
# with no wake; bind's check of a named socket made before it waits for the
# lock; a stream send judged after its copy as before and a datagram send
# not at all; a receive's descriptors in flight not found by the holder
# count while it installs or closes them, the one being closed counted as
# its own file's holder, or one it installed still counted; a descriptor in
# flight whose file moved counted, installed or released by the node it had
# before; and a descriptor in flight taken from its place before its drop
# has the root's lock. Each compiled fault must fail its named row and
# end the run with status 1; each restored source is touched, rebuilt and
# must pass again under ASan and UBSan.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
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

ipc = Path(sys.argv[1])
release = '            fd_release_backing(fd_get(table(), %s));\n            fd_free(table(), %s);\n'
faults = {
    'accept': ('unix.c', [
        ('    s->acc_len--;\n    wq_wake(&s->wq, 0);\n',
         '    s->acc_len--;\n    wq_wake(&s->wq, 0);\n    publish(fd, c, flags);\n'),
        ('            fd_free(table(), fd);\n            unix_release(c, 0);\n',
         release % ('fd', 'fd'))]),
    'socketpair': ('unix.c', [
        ('    rc = fds[1] < 0 ? fds[1] : 0;\n',
         '    rc = fds[1] < 0 ? fds[1] : 0;\n    if (rc == 0) {\n'
         '        publish(fds[0], a, flags);\n        publish(fds[1], b, flags);\n    }\n'),
        ('            fd_free(table(), fds[1]);\n', release % ('fds[1]', 'fds[1]')),
        ('            fd_free(table(), fds[0]);\n', release % ('fds[0]', 'fds[0]')),
        ('        unix_release(a, 0);\n        unix_release(b, 0);\n        return rc;\n',
         '        return rc;\n')]),
    'rights': ('unix.c', [
        ('        if (copy_to_user((void __user *)(at + CMSG_HDR + 4u * given), &fd, '
         'sizeof(fd)) != 0) {\n',
         '        table()->entries[fd].type = in->fds[given].type;\n'
         '        if (copy_to_user((void __user *)(at + CMSG_HDR + 4u * given), &fd, '
         'sizeof(fd)) != 0) {\n')]),
    'pipe2': ('pipe.c', [
        ('    if (kfds[1] < 0 || copy_to_user(',
         '    if (kfds[1] >= 0) {\n'
         '        pipe_setup_end(kfds[0], pipe, FD_FLAG_READ, flags);\n'
         '        pipe_setup_end(kfds[1], pipe, FD_FLAG_WRITE, flags);\n    }\n'
         '    if (kfds[1] < 0 || copy_to_user(')]),
    'connect': ('unix.c', [
        ('    if (s->state != US_UNCONNECTED) {\n'
         '        return s->state == US_CONNECTED ? -EISCONN : -EINVAL;\n    }\n', '')]),
    'bind': ('unix.c', [('        rc = bind_lock(s);\n', '        rc = 0;\n')]),
    'bind-wake': ('unix.c', [
        ('    s->binding = 0;\n    wq_wake_all(&s->lock_wq);\n', '    s->binding = 0;\n')]),
    'bind-check': ('unix.c', [
        ('    if (s->bound || s->closed) {\n        return -EINVAL;\n    }\n'
         '    if (n->len == 0) {\n', '    if (n->len == 0) {\n'),
        ('    if (rc == 0) {\n        rc = bind_lock(s);\n    }\n',
         '    if (rc == 0 && (s->bound || s->closed)) {\n        rc = -EINVAL;\n    }\n'
         '    if (rc == 0) {\n        rc = bind_lock(s);\n    }\n')]),
    'receive-wake': ('unix.c', [
        ('    s->reading = 0;\n    wq_wake_all(&s->lock_wq);\n', '    s->reading = 0;\n')]),
    'receive': ('unix.c', [
        ('    while (s->reading) {\n        wq_wait_event_nointr(&s->lock_wq, recv_idle, s);\n',
         '    while (0 && s->reading) {\n'
         '        wq_wait_event_nointr(&s->lock_wq, recv_idle, s);\n')]),
    'send-stream': ('unix.c', [
        ('        if (s->peer != p || s->err || send_stopped(s, p) || queue_full(p)) {\n',
         '        if (s->peer != p || p->closed || queue_full(p)) {\n')]),
    'send-datagram': ('unix.c', [
        ('        if (s->shut & SHUT_SEND) {\n            rc = -EPIPE;\n            break;\n'
         '        }\n', '')]),
    'transit-count': ('unix.c', [
        ('    for (in = transits; in; in = in->transit) {\n',
         '    for (in = NULL; in; in = in->transit) {\n')]),
    'transit-release': ('unix.c', [
        ('    if (count) {\n        (*count)--;\n    } else {\n        slot->type = FD_TYPE_NONE;\n'
         '    }\n    flight_sync(&e);\n    fd_release_backing(&e);\n',
         '    flight_sync(&e);\n    fd_release_backing(&e);\n    if (count) {\n'
         '        (*count)--;\n    } else {\n        slot->type = FD_TYPE_NONE;\n    }\n')]),
    'transit-installed': ('unix.c', [
        ('        in->fds[given++].type = FD_TYPE_NONE;\n', '        given++;\n')]),
    'moved-count': ('unix.c', [
        ('        if (e->type == FD_TYPE_FILE && e->node && e->ofd && e->ofd->vnode == node) {\n',
         '        if (e->type == FD_TYPE_FILE && e->node == node) {\n')]),
    'moved-install': ('unix.c', [('        flight_sync(slot);\n', '')]),
    'moved-release': ('unix.c', [
        ('    flight_sync(&e);\n    fd_release_backing(&e);\n', '    fd_release_backing(&e);\n')]),
    'flight-left-unlocked': ('unix.c', [
        ('    struct vfs_mount *locked = fd_release_lock(slot);\n    fd_entry_t e = *slot;\n\n'
         '    if (count) {\n        (*count)--;\n    } else {\n        slot->type = FD_TYPE_NONE;\n'
         '    }\n',
         '    struct vfs_mount *locked;\n    fd_entry_t e = *slot;\n\n'
         '    if (count) {\n        (*count)--;\n    } else {\n        slot->type = FD_TYPE_NONE;\n'
         '    }\n    locked = fd_release_lock(&e);\n')]),
}
name, changes = faults[sys.argv[2]]
path = ipc / name
source = path.read_text()
for old, new in changes:
    if source.count(old) != 1:
        raise SystemExit('unix-proof: a pattern for ' + sys.argv[2] + ' is not unique')
    source = source.replace(old, new)
path.write_text(source)
PY
}

faults='accept socketpair rights pipe2 connect bind bind-wake bind-check
    receive receive-wake send-stream send-datagram transit-count
    transit-release transit-installed moved-count moved-install moved-release
    flight-left-unlocked'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/ipc "$fault" >/dev/null || exit 1
    done
    echo 'unix-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" unix-proof "$repo/tests/host/unix_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/unix-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/ipc/unix.c" "$work/unix.saved"
cp "$work/sys/ipc/pipe.c" "$work/pipe.saved"

restore()
{
    cp "$work/unix.saved" "$work/sys/ipc/unix.c"
    cp "$work/pipe.saved" "$work/sys/ipc/pipe.c"
    touch "$work/sys/ipc/unix.c" "$work/sys/ipc/pipe.c"
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

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/unix_hosttest.c tests/host/compat/park_host.c \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/ipc/unix.c" "$work/sys/ipc/pipe.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "unix-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "unix-proof: result: $phase status $status"
}

echo "unix-proof: logs: $work"
run_phase fixed 0 'unix-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$work/sys/ipc" "$fault"
    touch "$work/sys/ipc/unix.c" "$work/sys/ipc/pipe.c"
    case $fault in
        accept) message="a sibling's close of the number accept reserved finds nothing" ;;
        socketpair) message="a sibling's close of the numbers socketpair reserved finds nothing" ;;
        rights) message="a sibling's close of the number a receive reserved finds nothing" ;;
        pipe2) message="a sibling's close of the numbers pipe2 reserved finds nothing" ;;
        connect) message='a connect whose copy of the address sleeps while a sibling connects' ;;
        bind) message='a second bind of a socket waits for the bind lock' ;;
        bind-wake) message="the first bind's end wakes the bind that waits" ;;
        bind-check) message='the bind that waited goes on from its wait, finds the socket named' ;;
        receive) message="a datagram receive while another's copy of a message sleeps waits" ;;
        receive-wake) message="the end of a receive's hold of the lock wakes the receive that" ;;
        send-stream) message='a stream send whose copy sleeps while the socket is shut down' ;;
        send-datagram) message='a datagram send whose copy sleeps while the socket is shut down' ;;
        transit-count) message='a file is not freed by the close of its last descriptor while a receive installs' ;;
        transit-release) message="the release of a descriptor in flight that is its file's last holder frees the file" ;;
        transit-installed) message='a descriptor a receive has installed is not counted in flight' ;;
        moved-count) message='a descriptor in flight is counted by the node its file moved to' ;;
        moved-install) message='a descriptor received after its file moved is installed with the node' ;;
        moved-release) message='a descriptor in flight never received releases the node its file moved to' ;;
        flight-left-unlocked) message='a descriptor in flight is counted until its drop has the root' ;;
    esac
    run_phase "$fault-mutated" 1 "unix-hosttest: fail: $message"
    restore
    run_phase "$fault-restored" 0 'unix-hosttest: result: pass:'
    cmp "$work/unix.saved" "$work/sys/ipc/unix.c"
    cmp "$work/pipe.saved" "$work/sys/ipc/pipe.c"
done
echo 'unix-proof: result: pass: every fault failed its row and the restored source passed'
