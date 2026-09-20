#!/bin/sh
# Compile mounted KRFFS before, during and after each fault.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffsmount-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/fs/krffsmount.c" "$work/krffsmount.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/krffsmount.saved" "$work/sys/fs/krffsmount.c"
    touch "$work/sys/fs/krffsmount.c"
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
        -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/krffsmount_hosttest.c "$work/sys/fs/fd.c" \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/krffsmount.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "krffsmount-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "krffsmount-mutation: result: $phase status $status"
}

echo "krffsmount-mutation: logs: $work"
run_phase fixed 0 'krffsmount-hosttest: result: pass:'
for fault in read-identity write-identity truncate-identity write-repoint truncate-repoint write-failure truncate-failure flush readonly directory; do
    python3 - "$work/sys/fs/krffsmount.c" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
if fault.endswith("-identity"):
    verb = fault.split("-")[0]
    start = source.index("static int32_t kv_" + verb + "(" if verb != "truncate"
                         else "static int kv_truncate(")
    end = source.index("\n}", start)
    body = source[start:end]
    if verb == "truncate":
        body = body.replace("    int rc = begin_write(mnt);",
                            "    uint64_t before = follow_moves(handle);\n"
                            "    int rc = begin_write(mnt);")
        body = body.replace("uint64_t was = follow_moves(handle);", "uint64_t was = before;")
    else:
        body = body.replace("    uint64_t at;", "    uint64_t at = follow_moves(handle);")
        body = body.replace("    at = follow_moves(handle);\n", "")
    if body == source[start:end]:
        raise SystemExit("krffsmount-mutation: identity pattern not found")
    source = source[:start] + body + source[end:]
else:
    changes = {
        "directory": ("dir = follow_moves(handle);", "dir = offset_of(mnt, handle);"),
        "write-failure": ("if (at != KRFFS_NO_NODE && at != was) {",
                          "if (written >= 0 && at != KRFFS_NO_NODE && at != was) {"),
        "truncate-failure": ("if (now != KRFFS_NO_NODE && now != was) {",
                             "if (rc == 0 && now != KRFFS_NO_NODE && now != was) {"),
        "write-repoint": ("vfs_repoint(mnt, handle_of(was), handle_of(at));",
                          "vfs_repoint(mnt, handle, handle_of(at));"),
        "truncate-repoint": ("vfs_repoint(mnt, handle_of(was), handle_of(now));",
                             "vfs_repoint(mnt, handle, handle_of(now));"),
        "flush": ("block_flush(vol->dev) != 0 || krffs_set_dirty(&vol->fs, 0)",
                  "krffs_set_dirty(&vol->fs, 0)"),
        "readonly": ("    (void)handle;\n    vfs_ns_lock(mnt);",
                     "    (void)handle;\n    if (begin_write(mnt) != 0) {\n"
                     "        return 0;\n    }"),
    }
    old, new = changes[fault]
    if source.count(old) != 1:
        raise SystemExit("krffsmount-mutation: source pattern is not unique")
    source = source.replace(old, new)
path.write_text(source)
PY
    case $fault in
        read-identity) message='waiting read follows two moves despite offset reuse' ;;
        write-identity|truncate-identity) message='second relocation updates target length' ;;
        write-repoint|truncate-repoint) message='every description repointed from resolved offset' ;;
        write-failure|truncate-failure) message='failed relocation repoints every description' ;;
        flush) message='earlier data reaches medium before clean flag' ;;
        readonly) message='read-only sync reports pending write failure' ;;
        directory) message='directory read resolves its open handle' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'krffsmount-hosttest: result: pass:'
    cmp "$work/krffsmount.saved" "$work/sys/fs/krffsmount.c"
done
