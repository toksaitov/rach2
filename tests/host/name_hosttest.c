/**
 * name_hosttest.c: the capture of a name record by an open without the big
 * lock, `vfs_name_capture_at` in rach2/sys/fs/name.c, across a change of
 * names.
 *
 * host-deps: rach2/sys/fs/name.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c
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
    return m;
}

/* No row reads a record's path or compares its nodes. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    (void)st;
    __builtin_trap();
}

int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    (void)mnt;
    (void)node;
    (void)out;
    (void)cap;
    __builtin_trap();
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
static int dir_node;

/** Run the rows; the count of failures, at most 100. */
int main(void)
{
    struct vfs_ref parent;
    struct vfs_name *name, *other;
    uint32_t seq, now;

    held_ops.hold = model_hold;
    held_ops.put = model_put;
    held_type.ops = &held_ops;
    bare_type.ops = &bare_ops;
    held_mnt.type = &held_type;
    bare_mnt.type = &bare_type;
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
