#!/bin/sh
# Compile the production resolver and open path with a controlled flush wait.
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

work = Path(sys.argv[1])


def swap(body, old, new):
    if old not in body:
        raise SystemExit("krffs-open: mutation pattern is not in its function: " + old.strip())
    return body.replace(old, new)


path = work / "sys/syscall_fs.c"
text = path.read_text()
old = "VFS_WALK_LOCK |"
start = text.index("static int32_t do_open(")
end = text.index("\n}", start)
body = text[start:end]
if body.count(old) != 1:
    raise SystemExit("krffs-open: mutation pattern is not unique")
body = swap(body, old, "0 |")
body = swap(body, "            vfs_walk_unlock(ref.mnt);\n", "")
body = swap(body, "            vfs_walk_unlock(locked);\n", "")
body = swap(body, "    vfs_walk_unlock(locked);", "    (void)locked;")
path.write_text(text[:start] + body + text[end:])
if sys.argv[2] == "late-lock":
    path = work / "sys/fs/krffsmount.c"
    text = path.read_text()
    start = text.index("static int kv_open(")
    end = text.index("\n}", start)
    body = text[start:end]
    body = swap(body, "struct krffs_open *o = open_at(vol, at);", "struct krffs_open *o;")
    body = swap(body, "    (void)flags;", "    (void)flags;\n    vfs_ns_lock(mnt);\n"
                        "    o = open_at(vol, at);")
    body = swap(body, "            return -ENOMEM;", "            vfs_ns_unlock(mnt);\n"
                        "            return -ENOMEM;")
    body = swap(body, "    return 0;", "    vfs_ns_unlock(mnt);\n    return 0;")
    path.write_text(text[:start] + body + text[end:])
PY
}

faults='open-lock late-lock'
if [ "${1:-}" = --texts ]; then
    # The faults name their files as the private tree lays them out.
    root=$(mktemp -d "${TMPDIR:-/tmp}/rach2-krffs-open-texts.XXXXXX")
    trap 'rm -rf "$root"' EXIT
    ln -s "$repo/rach2/sys" "$root/sys"
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$root" "$fault" >/dev/null || exit 1
    done
    echo 'krffs-open: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" krffs-open "$repo/tests/host/krffs_open_mutate.sh" "$@"

[ "$#" -eq 0 ] || [ "$*" = --check ] || exit 2
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-open-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/fs/vfs.c" "$work/vfs.saved"
cp "$work/sys/fs/krffsmount.c" "$work/krffsmount.saved"
cp "$work/sys/syscall_fs.c" "$work/syscall.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/vfs.saved" "$work/sys/fs/vfs.c"
    cp "$work/krffsmount.saved" "$work/sys/fs/krffsmount.c"
    cp "$work/syscall.saved" "$work/sys/syscall_fs.c"
    touch "$work/sys/fs/vfs.c" "$work/sys/syscall_fs.c" "$work/sys/fs/krffsmount.c"
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
    python3 - "$work" <<'PY'
from pathlib import Path
import re
import sys

work = Path(sys.argv[1])
groups = [
    ("fs/vfs.c", "vfs_open.inc", "vfs_mount_gone mount_base mount_rdonly vfs_ref_hold vfs_ref_put "
     "vfs_enter vfs_leave "
     "ns_my_turn vfs_ns_lock vfs_ns_unlock in_groups may_search toward_cwd mount_covering path_last "
     "mount_named_at mount_named named_in_rest node_put walk_lock walk_unlock vfs_walk_unlock "
     "ref_move_node "
     "walk_move walk_root root_current dotdot_stays walk_leaf walk_link walk_last_comp walk_is_link "
     "resolve_path vfs_resolve vfs_ref_release vfs_desc_open vfs_desc_close "
     "vfs_getattr vfs_open_node vfs_release vfs_truncate vfs_create"),
    ("syscall_fs.c", "syscall_open.inc", "name_is device_name open_device_name open_link_target "
     "node_make open_create "
     "open_mounted_node ofd_record_path open_install open_synth_node do_open"),
]
for source, output, names in groups:
    text = (work / "sys" / source).read_text()
    parts = []
    if source == "syscall_fs.c":
        parts = re.findall(r"^#define (?:O_|CRED_|S_IF).*", text, re.M)
        shared = (work / "sys" / "syscall_internal.h").read_text()
        parts += re.findall(r"^#define CRED_.*", shared, re.M)
    for name in names.split():
        matches = list(re.finditer(r"^[A-Za-z_][\w *]*\b" + name + r"\([^;]*?\n\{", text, re.M))
        if len(matches) != 1:
            raise SystemExit("krffs-open: function is not unique: " + name)
        match = matches[0]
        depth = 0
        tokens = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
        for token in re.finditer(tokens, text[match.end() - 1:], re.S):
            if token.group() == "{":
                depth += 1
            elif token.group() == "}":
                depth -= 1
                if depth == 0:
                    parts.append(text[match.start():match.end() - 1 + token.end()])
                    break
        else:
            raise SystemExit("krffs-open: unterminated function: " + name)
    (work / output).write_text("\n\n".join(parts) + "\n")
PY
    # The thread glue is compiled on its own, with none of the kernel's
    # include directories: glibc's <pthread.h> asks for <sched.h>, which
    # `-I $work/sys/proc` would answer with the kernel's, and the kernel's
    # 32-bit size_t then collides with the host's.
    if ! "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
        -c tests/host/fixtures/krffs_open_threads.c -o "$work/threads.o" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
        -I "$work" -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/fixtures/krffs_open.c "$work/threads.o" \
        "$work/sys/fs/fd.c" "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >>"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/vfs.c" "$work/sys/syscall_fs.c" "$work/sys/fs/krffsmount.c" \
        "$work/vfs_open.inc" "$work/syscall_open.inc" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "krffs-open: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "krffs-open: result: $phase status $status"
}

echo "krffs-open: logs: $work"
run_phase fixed 0 'krffs-open: result: pass:'
[ "${1:-}" != --check ] || exit 0
for fault in $faults; do
    apply_fault "$work" "$fault"
    message='original open follows second move despite copy reuse'
    if [ "$fault" = late-lock ]; then
        message='concurrent open follows second move despite copy reuse'
    fi
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'krffs-open: result: pass:'
    cmp "$work/vfs.saved" "$work/sys/fs/vfs.c"
    cmp "$work/syscall.saved" "$work/sys/syscall_fs.c"
    cmp "$work/krffsmount.saved" "$work/sys/fs/krffsmount.c"
done
