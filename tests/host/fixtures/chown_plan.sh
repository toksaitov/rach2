#!/bin/sh
# chown_plan.sh: extract the set-id rules of a mounted chown, unchanged, from
# copies of their sources into the file chown_plan.c includes.
#
# Usage: sh tests/host/fixtures/chown_plan.sh DIR
# DIR holds copies of rach2/sys/syscall_fs.c, rach2/sys/proc/process.h,
# rach2/sys/fs/vfs.h and rach2/sys/lib/kerrno.h under their base names. The
# script writes DIR/chown_plan.inc: the `VFS_SET_` masks, `EPERM`, `struct
# vfs_stat`, the `proc_privileged` macro, and the functions `cred_privileged`,
# `cred_in_suppl`, `cred_in_group` and `vfs_chown_plan`. It exits 1 when one
# of them is not found once.
set -eu
[ "$#" -eq 1 ] || {
    echo 'chown-plan: usage: chown_plan.sh DIR' >&2
    exit 2
}
exec python3 - "$1" <<'PY'
from pathlib import Path
import re
import sys

work = Path(sys.argv[1])


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('chown-plan: the function is not unique: ' + name)
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
    raise SystemExit('chown-plan: the function does not end: ' + name)


source = (work / 'syscall_fs.c').read_text()
proc = (work / 'process.h').read_text()
vfs = (work / 'vfs.h').read_text()
errno = (work / 'kerrno.h').read_text()
masks = re.findall(r'^#define VFS_SET_\w+\s+\S+', vfs, re.M)
eperm = re.findall(r'^#define EPERM\s+\S+', errno, re.M)
stat = re.findall(r'^struct vfs_stat \{.*?\n\};', vfs, re.M | re.S)
privileged = re.findall(r'^#define proc_privileged\(p\) .*$', proc, re.M)
if not masks or len(eperm) != 1 or len(stat) != 1 or len(privileged) != 1:
    raise SystemExit('chown-plan: the masks, EPERM, struct vfs_stat or proc_privileged were not found once')
parts = ['\n'.join(masks), eperm[0], stat[0], privileged[0]]
for name in ('cred_privileged', 'cred_in_suppl', 'cred_in_group', 'vfs_chown_plan'):
    parts.append(function(source, name))
(work / 'chown_plan.inc').write_text('\n\n'.join(parts) + '\n')
PY
