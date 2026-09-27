#!/bin/sh
# Compile each old fault privately, then restore and rebuild before passing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-inode-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp tests/system/runner/harness.c "$work/harness.c"
for source in sys/fs/krffs.c sys/syscall_fs.c harness.c; do
    cp "$work/$source" "$work/$(basename "$source").saved"
done
restore()
{
    for source in sys/fs/krffs.c sys/syscall_fs.c harness.c; do
        cp "$work/$(basename "$source").saved" "$work/$source"
        touch "$work/$source"
    done
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

# Extract whole production functions, keeping their bodies unchanged. The
# fixture supplies user copies and a resolver backed by the real KRFFS walk.
cat > "$work/extract.py" <<'PY'
from pathlib import Path
import re
import sys
w = Path(sys.argv[1])
repo = Path(sys.argv[2])
def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    assert len(matches) == 1, name
    m = matches[0]
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[m.end()-1:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return text[m.start():m.end()-1+token.end()]
    raise ValueError(name)
host = (repo / 'tests/host/krffs_hosttest.c').read_text()
host = host.replace('../../rach2/sys/fs/krffs.c', str(w / 'sys/fs/krffs.c'))
host = host.replace('"krffs_hash_vectors.h"', '"' + str(repo / 'tests/host/krffs_hash_vectors.h') + '"')
(w / 'host.c').write_text(host)
s = (w / 'sys/syscall_fs.c').read_text()
v = (w / 'sys/fs/vfs.c').read_text()
vh = (w / 'sys/fs/vfs.h').read_text()
parts = ['#define main core_main\n#include "host.c"\n#undef main']
for name, text in [('linux_stat', s), ('linux_stat64', s), ('vfs_stat', vh)]:
    parts.append(re.search(r'struct ' + name + r' \{.*?\n\};', text, re.S).group())
parts.append('''
#define __user
#define EFAULT 14
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define VFS_S_IFDIR S_IFDIR
#define VFS_S_IFREG S_IFREG
#define VFS_WALK_NOFOLLOW 1
static int current_proc = 1;
struct vfs_mount { int unused; };
struct vfs_ref { struct vfs_mount *mnt; void *node; };
struct vpath { int unused; };
#define AT_FDCWD (-100)
static char path_page[4096];
static int path_from_user(int dirfd, const char *in, char **out)
{ unsigned n = k_strlen(in) + 1; (void)dirfd; if (n > sizeof(path_page)) return -1;
  k_memcpy(path_page, in, n); *out = path_page; return 0; }
static void path_buf_put(char *buf) { (void)buf; }
static int copy_to_user(void *out, const void *in, unsigned n)
{ k_memcpy(out, in, n); return 0; }
static int vfs_resolve(int a, const char *p, int f, struct vfs_ref *r, int b, int c)
{ (void)a; (void)f; (void)b; (void)c; r->mnt = 0; r->node = krffs_resolve(p); return r->node ? 0 : -1; }
static const struct vpath *vfs_synth_vpath(struct vfs_mount *m, void *n)
{ (void)m; (void)n; return 0; }
static int mnt_is_root(struct vfs_mount *m) { (void)m; return 1; }
static void vfs_ref_release(struct vfs_ref *r) { (void)r; }
static int synth_stat(const struct vpath *p, struct linux_stat64 *s, int f)
{ (void)p; (void)s; (void)f; return -1; }
static int vfs_fill_stat64(struct linux_stat64 *s, const struct vfs_ref *r)
{ (void)s; (void)r; return -1; }
static void stat64_boot_times(struct linux_stat64 *s) { (void)s; }
''')
for name in ['stat64_node_times', 'stat64_root_times', 'krffs_fill_stat64',
             'stat_times_from_stat64', 'stat_node_times', 'stat_root_times',
             'do_stat64', 'do_old_stat']:
    parts.append(function(s, name))
parts.append(function(v, 'kfs_getattr'))
parts.append('''
int main(void)
{
    const char *paths[] = {"/", "/tmp/.."};
    unsigned i;
    if (core_main()) return 1;
    make_image(image, IMAGE_SIZE);
    ((krffs_node_t *)image)->mode = 0755;
    check(init_krffs(image, IMAGE_SIZE) == 0 && krffs_mkdir("/tmp") == 0, "root stat fixture");
    for (i = 0; i < 2; i++) {
        struct linux_stat old;
        struct linux_stat64 wide;
        struct vfs_stat attr;
        check(do_old_stat(paths[i], &old, 1) == 0 && old.st_ino == 1 &&
              (old.st_mode & 0170000) == S_IFDIR, "root old stat inode is 1");
        check(do_stat64(paths[i], &wide, 1) == 0 && wide.st_ino == 1 && wide.__st_ino == 1,
              "root stat64 inode is 1");
        krffs_fill_stat64(&wide, krffs_resolve(paths[i]));
        check(wide.st_ino == 1 && wide.__st_ino == 1, "root node stat64 inode is 1");
        check(kfs_getattr(0, krffs_resolve(paths[i]), &attr) == 0 && attr.ino == 1,
              "root VFS inode is 1");
    }
    if (fails) return 1;
    printf("inode-proof: root stat paths pass\\n");
    return 0;
}
''')
(w / 'stat.c').write_text('\n\n'.join(parts))
PY
run_phase()
{
    phase=$1
    kind=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    if [ "$kind" = harness ]; then
        "${CC:-cc}" -O2 -Wall -Wextra -Werror "$work/harness.c" -o "$binary" >"$binary.compile.log" 2>&1 || {
            cat "$binary.compile.log"; exit 1;
        }
        set -- "$binary" --selftest
    else
        python3 "$work/extract.py" "$work" "$repo"
        "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
            -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
            "$work/stat.c" "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
            -o "$binary" >"$binary.compile.log" 2>&1 || {
                cat "$binary.compile.log"; exit 1;
            }
        set -- "$binary"
    fi
    cksum "$work/sys/fs/krffs.c" "$work/sys/syscall_fs.c" "$work/harness.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 15; exec @ARGV or die "inode-proof: exec: $!\n"' \
        "$@" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "inode-proof: result: $phase status $status"
}
echo "inode-proof: logs: $work"
run_phase fixed inode 0 'root stat paths pass'
run_phase harness-fixed harness 0 'probe wait bounds pass'
for fault in orphan root-node root-old root-wide harness; do
    python3 - "$work" "$fault" <<'PY'
from pathlib import Path
import sys
w = Path(sys.argv[1])
changes = {
'orphan': ('sys/fs/krffs.c', 'return krffs_mix_hash(node->parent_id, node->id);',
           'return krffs_mix_id(node->parent_id, (const char *)node->name);'),
'root-node': ('sys/fs/krffs.c', '    if (node->type == KRFFS_ROOT_NODE) {\n        return 1;\n    }\n', ''),
'root-old': ('sys/syscall_fs.c', '        kst.st_ino = 1;\n', '        kst.st_ino = 0;\n'),
'root-wide': ('sys/syscall_fs.c', '            kst.st_ino = kst.__st_ino = 1;\n', '            kst.st_ino = 0;\n'),
'harness': ('harness.c', '    if (child_poll() >= 0)\n        drain_ready();\n', '')}
file, old, new = changes[sys.argv[2]]
p = w / file
s = p.read_text()
assert s.count(old) == 1, sys.argv[2]
p.write_text(s.replace(old, new))
PY
    kind=inode
    message='root inode is 1'
    case "$fault" in
        orphan) message='open unlinked siblings keep distinct unchanged inode numbers' ;;
        root-old) message='root old stat inode is 1' ;;
        root-wide) message='root stat64 inode is 1' ;;
        harness) kind=harness; message='returned 0, wanted 1' ;;
    esac
    run_phase "$fault-mutated" "$kind" 1 "$message"
    restore
    if [ "$kind" = harness ]; then message='probe wait bounds pass'; else message='root stat paths pass'; fi
    run_phase "$fault-restored" "$kind" 0 "$message"
done
