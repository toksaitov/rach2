/**
 * name_hosttest.c: capture a directory entry and reconstruct its path
 * across changes of names with the big lock released.
 *
 * host-deps: rach2/sys/fs/name.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * The open that the name cache finishes reads the count of changes of names
 * before its walk (`vfs_name_seq`) and captures the record of the entry it
 * found after (`vfs_name_capture_at`); an unlink or a rename brackets its
 * change (`vfs_name_change_begin`, `vfs_name_change_end`). Rows check that a
 * capture with no change since the count succeeds and holds the parent and
 * its mount; that one while a change is under way, or after one began and
 * ended since the count, is refused with -EAGAIN, nothing kept and every
 * hold given back, so the open goes to the full walk, as Linux's walk under
 * RCU retries when `rename_lock` moved (fs/namei.c:2071, 2501); that the
 * count says a change is under way until it ends; that the capture of the
 * walk under the big lock (`vfs_name_capture`) is not refused; and that a
 * type without `hold` and `put` gets no record. The parent's holds and its
 * mount's references are counted by models of the VFS's calls; the test
 * returns the number of failed checks.
 */
#include "fs/name.h"
#include "fs/vfs.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"

int printf(const char *format, ...);

static int fails;

/** Count a failure when `cond` is false. */
#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* The holds of the parent directory and the references of its mount that
 * the records keep, by the models below. */
static int holds, opens;

void vfs_ref_hold(const struct vfs_ref *r)
{
    (void)r;
    holds++;
}

void vfs_ref_put(struct vfs_ref *r)
{
    (void)r;
    holds--;
}

void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
    opens++;
}

/** The end of a record's hold, as vfs.c's: the parent's hold put and the
 * mount's reference dropped. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    (void)node;
    (void)handle;
    holds--;
    opens--;
}

const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m->bind_of ? m->bind_of : m;
}

/* Each model inode has a single address. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    (void)st;
    __builtin_trap();
}

static int dir_node, file_node, root_node;
static const char *file_leaf;
static char parent_path[] = "/r/p/a";
static int scans;

/** Look up the one live name in directory a and hold its inode. */
static int model_lookup(struct vfs_mount *mnt, void *parent, const char *leaf, void **node)
{
    (void)mnt;
    if (parent != &dir_node || __builtin_strcmp(leaf, file_leaf) != 0) {
        return -ENOENT;
    }
    *node = &file_node;
    holds++;
    return 0;
}

/** While the first scan reads p, rename file to new in a, then r to s at
 * the root. A second scan must exclude changes through the base mount. */
int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    struct vfs_mount *base = mnt->bind_of;

    CHECK(node == &dir_node);
    if (++scans == 1) {
        CHECK(base->name_walk.state == 0);
        vfs_name_lock_changes(base);
        CHECK(base->name_walk.state == 1);
        vfs_name_change_begin();
        file_leaf = "new";
        vfs_name_rename(base, &dir_node, "file", &dir_node, file_leaf);
        vfs_name_change_end();
        vfs_name_unlock_changes(base);
        vfs_name_lock_changes(base);
        vfs_name_change_begin();
        parent_path[1] = 's';
        vfs_name_rename(base, &root_node, "r", &root_node, "s");
        vfs_name_change_end();
        vfs_name_unlock_changes(base);
    } else {
        CHECK(base->name_walk.state == RWSEM_WRITER);
    }
    if (sizeof(parent_path) > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out, parent_path, sizeof(parent_path));
    return 0;
}

/** The model type's `hold` and `put`, which a record needs its type to have;
 * the holds are counted by `vfs_ref_hold` and `vfs_desc_close`. */
static void model_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
}

/** The `put` that goes with `model_hold`. */
static void model_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
}

static struct vfs_ops held_ops, bare_ops;
static struct vfs_type held_type, bare_type;
static struct vfs_mount held_mnt, bare_mnt;

/** Read the selected entry through a bind while both renames use its base. */
static void rename_walk(void)
{
    struct vfs_mount bind = {0};
    struct vfs_ref parent;
    struct vfs_name *name;
    char path[32];
    int deleted, rc;

    bind.type = &held_type;
    bind.bind_of = &held_mnt;
    parent.mnt = &bind;
    parent.node = &dir_node;
    file_leaf = "file";
    rc = vfs_name_capture(&parent, file_leaf, &name);
    CHECK(rc == 0 && name);
    if (name) {
        rc = vfs_name_path(name, &file_node, path, sizeof(path), &deleted);
        CHECK(rc == 0 && !deleted && __builtin_strcmp(path, "/s/p/a/new") == 0);
        CHECK(scans == 2 && held_mnt.name_walk.state == 0);
        vfs_name_put(name);
        printf("name-hosttest: rename during walk: %s\n", rc ? "path lookup failed" : path);
    }
    CHECK(holds == 0 && opens == 0);
}

/** Run the rows, or just the sleeping walk for --rename-walk-only. */
int main(int argc, char **argv)
{
    struct vfs_ref parent;
    struct vfs_name *name, *other;
    uint32_t seq, now;

    held_ops.hold = model_hold;
    held_ops.put = model_put;
    held_ops.lookup = model_lookup;
    held_type.ops = &held_ops;
    held_type.file_locked = 1;
    bare_type.ops = &bare_ops;
    held_mnt.type = &held_type;
    bare_mnt.type = &bare_type;
    rename_walk();
    if (argc == 2 && __builtin_strcmp(argv[1], "--rename-walk-only") == 0) {
        return fails;
    }
    parent.mnt = &held_mnt;
    parent.node = &dir_node;

    /* No change since the count: the record is kept, with its holds. */
    CHECK(vfs_name_seq(&seq) == 1);
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == 0 && name != 0);
    CHECK(holds == 1 && opens == 1);
    vfs_name_put(name);
    CHECK(holds == 0 && opens == 0);

    /* A change under way: the count says so, and the capture is refused
     * with nothing kept. */
    CHECK(vfs_name_seq(&seq) == 1);
    vfs_name_change_begin();
    CHECK(vfs_name_seq(&now) == 0);
    name = (struct vfs_name *)&dir_node;
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == -EAGAIN && name == 0);
    CHECK(holds == 0 && opens == 0);
    CHECK(vfs_name_capture_at(&parent, "file", now, &name) == -EAGAIN && name == 0);
    CHECK(holds == 0 && opens == 0);

    /* The walk under the big lock captures across the change. */
    CHECK(vfs_name_capture(&parent, "file", &other) == 0 && other != 0);
    CHECK(holds == 1 && opens == 1);

    /* A change that began and ended since the count: still refused. */
    vfs_name_change_end();
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == -EAGAIN && name == 0);
    CHECK(holds == 1 && opens == 1);

    /* Nested changes: refused until the last ends. */
    CHECK(vfs_name_seq(&seq) == 1);
    vfs_name_change_begin();
    vfs_name_change_begin();
    vfs_name_change_end();
    CHECK(vfs_name_seq(&now) == 0);
    CHECK(vfs_name_capture_at(&parent, "file", now, &name) == -EAGAIN && name == 0);
    vfs_name_change_end();
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == -EAGAIN && name == 0);
    CHECK(holds == 1 && opens == 1);

    /* A count read after the change ends: the record is kept. */
    CHECK(vfs_name_seq(&seq) == 1);
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == 0 && name != 0);
    CHECK(holds == 2 && opens == 2);
    vfs_name_put(name);
    vfs_name_put(other);
    CHECK(holds == 0 && opens == 0);

    /* A type with no `hold` and `put` keeps no record and is not refused. */
    parent.mnt = &bare_mnt;
    vfs_name_change_begin();
    CHECK(vfs_name_capture_at(&parent, "file", seq, &name) == 0 && name == 0);
    vfs_name_change_end();
    CHECK(holds == 0 && opens == 0);
    return fails;
}
