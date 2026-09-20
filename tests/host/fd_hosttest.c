/**
 * Check description locking across a pending KRFFS relocation.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The namespace substitute completes the writer's relocation when the reader
 * waits for it. The operation takes the namespace recursively, as the root
 * adapter does. A handle captured before that wait still refers to the freed
 * node even though fd_ofd_repoint updated the description.
 */
#include "../../rach2/sys/fs/vfs.h"
#include "../../rach2/sys/mm/kheap.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

const struct vfs_type vfs_krffs_type = {0};
static const struct vfs_type other_type = {0};
static struct open_file *reader;
static int old_node, new_node;
static int pending, depth, waits, fails;

/** Record a failed invariant without hiding later checks. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("fd_hosttest: fail: %s\n", what);
        fails++;
    }
}

/** Supply the descriptor allocator with zeroed host memory. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Release memory allocated by the descriptor code. */
void kfree(void *ptr)
{
    free(ptr);
}

/** Initialize the unused scheduler queue. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = 0;
}

/** Complete an earlier operation on a shared description. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    check(!cond(arg), "description wait starts busy");
    check(depth == 1, "namespace precedes description wait");
    ((struct open_file *)arg)->busy = 0;
    check(cond(arg), "description wait ends idle");
    waits++;
    return 0;
}

/** Check that a wake follows the description's release. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    check(!reader->busy, "wake follows description release");
}

/** Complete the other description's relocation before granting the lock. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    if (pending) {
        check(!reader->busy, "namespace wait precedes description lock");
        fd_ofd_repoint(mnt, &old_node, &new_node);
        pending = 0;
        waits++;
    }
    depth++;
}

/** Check release order at the outer namespace lock. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(depth > 0, "namespace unlock is balanced");
    if (depth == 1) {
        check(!reader->busy, "description released before namespace");
    }
    depth--;
}

/** No backing resources exist in this lock test. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    (void)node;
    (void)handle;
}

/** Model the root adapter's by-value handle and recursive namespace lock. */
static void operation(struct vfs_mount *mnt, void *handle)
{
    vfs_ns_lock(mnt);
    check(handle == &new_node, "operation receives relocated handle");
    check(reader->vnode == &new_node, "relocation updates held node");
    vfs_ns_unlock(mnt);
}

/** Exercise a waiting independent open, a shared open, and other file types. */
int main(void)
{
    fd_table_t table;
    struct vfs_mount mnt = {0};
    int fd;

    if (fd_table_init(&table) != 0 || (fd = fd_alloc(&table)) < 0) {
        return 1;
    }
    reader = table.entries[fd].ofd;
    mnt.type = &vfs_krffs_type;
    reader->vmnt = &mnt;
    reader->vnode = &old_node;
    reader->fhandle = &old_node;
    pending = 1;
    fd_ofd_lock(reader);
    operation(reader->vmnt, reader->fhandle);
    fd_ofd_unlock(reader);
    check(waits == 1 && !pending && depth == 0, "relocation wait completed");

    reader->busy = 1;
    fd_ofd_lock(reader);
    fd_ofd_unlock(reader);
    check(waits == 2 && depth == 0, "shared description wait completed");

    mnt.type = &other_type;
    fd_ofd_lock(reader);
    check(depth == 0, "other file types take no root namespace lock");
    fd_ofd_unlock(reader);
    reader->vmnt = 0;
    fd_ofd_lock(reader);
    fd_ofd_unlock(reader);
    check(depth == 0 && !reader->busy, "raw description lock is balanced");
    fd_free(&table, fd);
    fd_table_free(&table);
    if (!fails) {
        printf("fd_hosttest: pass: namespace and description ordering\n");
    }
    return fails != 0;
}
