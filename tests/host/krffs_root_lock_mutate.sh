#!/bin/sh
# Compile the KRFFS root's data operations and the mount's lock, with and
# without the lock and its line.
#
# Usage: sh tests/host/krffs_root_lock_mutate.sh [--check]
#
# The five data operations of the root adapter in rach2/sys/fs/vfs.c, the
# bodies they call and the namespace lock, with the bind's base it takes, are
# extracted unchanged from a private copy of the source and compiled with the
# real KRFFS core into tests/host/fixtures/krffs_root_lock.c, under ASan and
# UBSan, with the thread glue of tests/host/fixtures/krffs_open_threads.c. The
# fixture requires each operation to hold the mount's lock wherever it reaches
# the image, and a holder that lets the lock go and asks again to wait behind
# a process already waiting (entry 104 of milestone 50). `--check` runs that
# once and stops; the host suite runs it so. The whole proof then takes the
# lock out of one operation at a time, and then takes the line out of the
# lock: each fault must fail its own row, and the restored copy, touched and
# rebuilt, must pass.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || exit 2
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-root-lock-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/fs/vfs.c" "$work/vfs.saved"

restore()
{
    cp "$work/vfs.saved" "$work/sys/fs/vfs.c"
    touch "$work/sys/fs/vfs.c"
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

# run_phase PHASE STATUS MESSAGE: extract the functions from the private
# vfs.c, compile the fixture over them, and run it; it must exit with STATUS
# and print MESSAGE.
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
text = (work / "sys/fs/vfs.c").read_text()
turn = re.search(r"^struct ns_turn \{.*?\n\};", text, re.M | re.S)
if not turn:
    raise SystemExit("krffs-root-lock: struct ns_turn is missing")
parts = [turn.group()]
names = ("mount_base ns_my_turn vfs_ns_lock vfs_ns_unlock kfs_read_locked kfs_write_locked "
         "kfs_truncate_locked kfs_fsync_locked kfs_readdir_locked kfs_read kfs_write "
         "kfs_truncate kfs_fsync kfs_readdir")
tokens = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
for name in names.split():
    matches = list(re.finditer(r"^[A-Za-z_][\w *]*\b" + name + r"\([^;]*?\n\{", text, re.M))
    if len(matches) != 1:
        raise SystemExit("krffs-root-lock: function is not unique: " + name)
    match = matches[0]
    depth = 0
    for token in re.finditer(tokens, text[match.end() - 1:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                parts.append(text[match.start():match.end() - 1 + token.end()])
                break
    else:
        raise SystemExit("krffs-root-lock: unterminated function: " + name)
(work / "kfs_data.inc").write_text("\n\n".join(parts) + "\n")
PY
    # The thread glue is compiled on its own, with none of the kernel's
    # include directories, as krffs_open_mutate.sh compiles it: glibc's
    # <pthread.h> asks for <sched.h>, which `-I $work/sys/proc` would answer
    # with the kernel's.
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
        tests/host/fixtures/krffs_root_lock.c "$work/threads.o" "$work/sys/lib/kstring.c" \
        "$work/sys/lib/kctypes.c" -o "$binary" >>"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/vfs.c" "$work/kfs_data.inc" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "krffs-root-lock: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "krffs-root-lock: result: $phase status $status"
}

echo "krffs-root-lock: logs: $work"
run_phase fixed 0 'krffs-root-lock: result: pass:'
[ "${1:-}" != --check ] || exit 0
for fault in read write truncate fsync readdir line; do
    python3 - "$work/sys/fs/vfs.c" "$fault" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
fault = sys.argv[2]
text = path.read_text()
if fault == "line":
    # The lock as it was before its line: free is enough, whoever asked
    # first.
    name = "ns_my_turn"
    changes = [("    return t->mnt->ns_holder == -1 && t->mnt->ns_serving == t->place;",
                "    return t->mnt->ns_holder == -1;")]
else:
    name = "kfs_" + fault
    changes = [("    vfs_ns_lock(mnt);\n", ""), ("    vfs_ns_unlock(mnt);\n", "")]
matches = list(re.finditer(r"^static [\w ]*?\b" + name + r"\([^;]*?\n\{", text, re.M))
if len(matches) != 1:
    raise SystemExit("krffs-root-lock: the function is not unique: " + name)
start = matches[0].start()
end = text.index("\n}\n", start)
body = text[start:end]
for old, new in changes:
    if body.count(old) != 1:
        raise SystemExit("krffs-root-lock: the pattern is not in " + name + " once")
    body = body.replace(old, new)
path.write_text(text[:start] + body + text[end:])
PY
    if cmp -s "$work/vfs.saved" "$work/sys/fs/vfs.c"; then
        echo "krffs-root-lock: the $fault mutation did not change the source" >&2
        exit 1
    fi
    if [ "$fault" = line ]; then
        message='fail: vfs_ns_lock serves a waiter before a holder that let the lock go and asked again'
    else
        message="fail: kfs_$fault holds the mount's lock wherever it reaches the image"
    fi
    run_phase "$fault-mutated" 1 "$message"
    restore
    run_phase "$fault-restored" 0 'krffs-root-lock: result: pass:'
    cmp "$work/vfs.saved" "$work/sys/fs/vfs.c"
done
echo 'krffs-root-lock: result: pass: each operation without its lock and the lock without its line failed, and each restoration passed'
