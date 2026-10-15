#!/bin/sh
# Prove the set-id rules of a chown on a mounted file system with private
# mutations of rach2/sys/syscall_fs.c and rach2/sys/proc/process.h.
# `vfs_chown_plan` and the credential helpers it calls, `cred_privileged`,
# `cred_in_suppl` and `cred_in_group`, are extracted unchanged from a private
# copy of rach2/sys/syscall_fs.c, with the `proc_privileged` macro from
# rach2/sys/proc/process.h, `struct vfs_stat` and the `VFS_SET_` masks from
# rach2/sys/fs/vfs.h and `EPERM` from rach2/sys/lib/kerrno.h, and compiled
# under ASan and UBSan in tests/host/fixtures/chown_plan.c.
#
# Usage: chown_mutate.sh [--check | --texts]
# With --check it compiles and runs the source as it is, which is what the
# host suite does; without, it also compiles each fault in, requires its row
# to fail, and requires the restored source to pass again. The faults: a
# directory losing its bits, setgid cleared whatever group-execute says, an
# owner allowed to give a file away, a group change judged by the file's
# group instead of the one asked for, and root of any user namespace taken
# as privileged.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, into vfs_chown_plan in a copy of
# syscall_fs.c or, for userns, into the proc_privileged macro in a copy of
# process.h; with RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
text = path.read_text()
changes = {
    'dir': ('    if ((vst->mode & 06000u) && (vst->mode & 0170000u) != 0040000u) {\n',
            '    if (vst->mode & 06000u) {\n'),
    'gexec': ('        if (vst->mode & 0010u) {\n            vst->mode &= ~02000u;\n        }\n',
              '        vst->mode &= ~02000u;\n'),
    'owner': ('(what & VFS_SET_UID) != 0 ||', '0 ||'),
    'group': ('!cred_in_group(gid)', '!cred_in_group(vst->gid)'),
    'userns': (' && !(p)->userns)', ')'),
}
old, new = changes[sys.argv[2]]
head = 'static int vfs_chown_plan(struct vfs_stat *vst, uint32_t uid, uint32_t gid)\n{'
if sys.argv[2] == 'userns':
    head = '#define proc_privileged(p) '
if text.count(head) != 1:
    raise SystemExit('chown-proof: the text the fault goes into is not unique')
start = text.index(head)
end = text.index('\n', start) if sys.argv[2] == 'userns' else text.index('\n}\n', start)
body = text[start:end]
if body.count(old) != 1:
    raise SystemExit('chown-proof: the pattern for ' + sys.argv[2] + ' is not unique')
path.write_text(text[:start] + body.replace(old, new) + text[end:])
PY
}

# fault_file FAULT: the source in the tree that FAULT goes into.
fault_file()
{
    case $1 in
        userns) echo rach2/sys/proc/process.h ;;
        *) echo rach2/sys/syscall_fs.c ;;
    esac
}

faults='dir gexec owner group userns'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$(fault_file "$fault")" "$fault" >/dev/null || exit 1
    done
    echo 'chown-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" chown-proof "$repo/tests/host/chown_mutate.sh" "$@"

[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'chown-proof: usage: chown_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/chown-proof.XXXXXX")
child=
cp rach2/sys/syscall_fs.c "$work/syscall_fs.c"
cp rach2/sys/proc/process.h "$work/process.h"
cp rach2/sys/fs/vfs.h "$work/vfs.h"
cp rach2/sys/lib/kerrno.h "$work/kerrno.h"
cp "$work/syscall_fs.c" "$work/syscall_fs.saved"
cp "$work/process.h" "$work/process.saved"

# restore: copy the saved syscall_fs.c and process.h back over the copies
# under test.
restore()
{
    cp "$work/syscall_fs.saved" "$work/syscall_fs.c"
    cp "$work/process.saved" "$work/process.h"
    touch "$work/syscall_fs.c" "$work/process.h"
}
# cleanup: the exit trap; stop the running test and restore the source.
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

# run_phase PHASE STATUS MESSAGE: extract the functions and definitions from
# the copies into $work/chown_plan.inc, compile fixtures/chown_plan.c over it
# as $work/PHASE, run it for at most 20 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    python3 - "$work" <<'PY'
from pathlib import Path
import re
import sys

work = Path(sys.argv[1])


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('chown-proof: the function is not unique: ' + name)
    match = matches[0]
    depth = 0
    tokens = r'/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for token in re.finditer(tokens, text[match.end() - 1:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[match.start():match.end() - 1 + token.end()]
    raise SystemExit('chown-proof: the function does not end: ' + name)


source = (work / 'syscall_fs.c').read_text()
proc = (work / 'process.h').read_text()
vfs = (work / 'vfs.h').read_text()
errno = (work / 'kerrno.h').read_text()
masks = re.findall(r'^#define VFS_SET_\w+\s+\S+', vfs, re.M)
eperm = re.findall(r'^#define EPERM\s+\S+', errno, re.M)
stat = re.findall(r'^struct vfs_stat \{.*?\n\};', vfs, re.M | re.S)
privileged = re.findall(r'^#define proc_privileged\(p\) .*$', proc, re.M)
if not masks or len(eperm) != 1 or len(stat) != 1 or len(privileged) != 1:
    raise SystemExit('chown-proof: the masks, EPERM, struct vfs_stat or proc_privileged were not found once')
parts = ['\n'.join(masks), eperm[0], stat[0], privileged[0]]
for name in ('cred_privileged', 'cred_in_suppl', 'cred_in_group', 'vfs_chown_plan'):
    parts.append(function(source, name))
(work / 'chown_plan.inc').write_text('\n\n'.join(parts) + '\n')
PY
    if ! "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" tests/host/fixtures/chown_plan.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/syscall_fs.c" "$work/process.h" "$work/chown_plan.inc" "$binary" >"$binary.cksum"
    perl -e 'alarm 20; exec @ARGV or die "chown-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "chown-proof: result: $phase status $status"
}

echo "chown-proof: logs: $work"
run_phase fixed 0 'chown-proof: result: pass:'
[ "$#" -eq 0 ] || exit 0
for fault in $faults; do
    file=$work/$(basename "$(fault_file "$fault")")
    apply_fault "$file" "$fault"
    touch "$file"
    case $fault in
        dir) message='a directory keeps its set-id bits across a change of owner' ;;
        gexec) message='setgid stays through a change of owner when group-execute is clear' ;;
        owner) message='an owner may not give a file away, and the refusal changes nothing' ;;
        group) message='an owner may not give a file to a group it is not in' ;;
        userns) message='root of another user namespace may not take a file it does not own' ;;
    esac
    run_phase "$fault-mutated" 1 "chown-proof: fail: $message"
    restore
    run_phase "$fault-restored" 0 'chown-proof: result: pass:'
    cmp "$work/syscall_fs.saved" "$work/syscall_fs.c"
    cmp "$work/process.saved" "$work/process.h"
done
echo 'chown-proof: result: pass: every fault failed its row and the restored source passed'
