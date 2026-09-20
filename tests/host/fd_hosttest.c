/**
 * Check standard description allocation and locking across relocation.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The namespace substitute completes the writer's relocation when the reader
 * waits for it. The operation takes the namespace recursively, as the root
 * adapter does. A handle captured before that wait still refers to the freed
 * node even though fd_ofd_repoint updated the description.
 */
#include "fs/vfs.h"
#include "mm/kheap.h"

/* The native process layout cannot satisfy the i386 compile-time offsets. */
#define __TINYC__ 1
#include "proc/process.c"
#undef __TINYC__

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern void exit(int status);
volatile uint32_t init_pid;

const struct vfs_type vfs_krffs_type = {0};
static const struct vfs_type other_type = {0};
static struct open_file *reader;
static int old_node, new_node;
static int pending, depth, waits, fails;
static int fail_after = -1;
static int allocations;

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
    void *ptr;

    if (fail_after == 0) {
        return 0;
    }
    if (fail_after > 0) {
        fail_after--;
    }
    ptr = calloc(1, size);
    if (ptr) {
        allocations++;
    }
    return ptr;
}

/** Release memory allocated by the descriptor code. */
void kfree(void *ptr)
{
    if (ptr) {
        allocations--;
    }
    free(ptr);
}

/** Use the same failure counter for process and descriptor allocations. */
void *kmalloc(size_t size)
{
    return kzalloc(size);
}

/** Supply the process allocator's initial floating-point image. */
void fpu_init_area(void *area)
{
    k_memset(area, 0, 512);
}

/** These processes never enter the scheduler. */
void sched_forget(process_t *p)
{
    (void)p;
}

/** These process fixtures allocate no virtual memory. */
void vm_free(vm_space_t *vm)
{
    (void)vm;
}

/** No row reparents a child. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    (void)signo;
    check(0, "unexpected signal");
}

/** Stop on any architecture check reached by mistake. */
void k_panic(const char *message)
{
    printf("fd_hosttest: fail: unexpected panic: %s\n", message);
    exit(1);
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

/** Fail each standard-description allocation and require a reusable table. */
static void stdio_allocations(void)
{
    fd_table_t table = {0};
    int fail, i, j;

    check(fd_init_stdio(0) == -1, "stdio rejects a missing table");
    check(fd_init_stdio(&table) == -1, "stdio rejects an unallocated table");
    for (fail = 0; fail < 3; fail++) {
        check(fd_table_init(&table) == 0, "stdio table allocation");
        fail_after = fail;
        check(fd_init_stdio(&table) == -1, "stdio reports allocation failure");
        fail_after = -1;
        check(allocations == 1, "stdio releases partial allocations");
        for (i = FD_STDIN; i <= FD_STDERR; i++) {
            fd_entry_t *e = &table.entries[i];

            check(e->type == FD_TYPE_NONE && !e->ofd && !e->flags,
                  "stdio failure leaves an empty table");
            fd_free(&table, i);
        }
        check(fd_init_stdio(&table) == 0, "stdio retry succeeds");
        check(allocations == 4, "stdio allocates three descriptions");
        for (i = FD_STDIN; i <= FD_STDERR; i++) {
            fd_entry_t *e = &table.entries[i];

            check(e->type == FD_TYPE_CONSOLE && e->ofd, "stdio descriptor has a description");
            check(e->flags == (i == FD_STDIN ? FD_FLAG_READ : FD_FLAG_WRITE), "stdio access mode");
            if (!e->ofd) {
                continue;
            }
            check(e->ofd->refcount == 1 && !e->ofd->offset && !e->ofd->raw_offset &&
                      !e->ofd->status_flags,
                  "stdio description starts cleared");
            for (j = FD_STDIN; j < i; j++) {
                check(e->ofd != table.entries[j].ofd, "stdio descriptions are separate");
            }
        }
        for (i = FD_STDIN; i <= FD_STDERR; i++) {
            fd_free(&table, i);
        }
        fd_table_free(&table);
        check(allocations == 0, "stdio final close releases every allocation");
    }
}

/** Release a fixture process and its standard descriptions. */
static void close_process(process_t *p)
{
    int i;

    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        fd_free(&p->fds, i);
    }
    free_process(p);
}

/** Fail each process allocation before the record can enter the live list. */
static void process_allocations(void)
{
    process_t *keeper = alloc_process();
    int fail;
    uint32_t forks = process_forks_since_boot();

    check(keeper != NULL, "initial process allocation");
    for (fail = 0; fail < 5; fail++) {
        process_t *p;

        fail_after = fail;
        p = alloc_process();
        fail_after = -1;
        check(!p, "process reports standard-description allocation failure");
        check(proc_list == keeper && proc_tail == keeper && !keeper->next,
              "failed process is not published");
        check(process_forks_since_boot() == forks, "failed process is not counted");
        check(allocations == 5, "failed process releases every allocation");
        if (p) {
            close_process(p);
            forks = process_forks_since_boot();
        }
    }
    close_process(keeper);
    check(!proc_list && !proc_tail && !allocations, "process cleanup releases all storage");
}

/** Exercise a waiting independent open, a shared open, and other file types. */
int main(void)
{
    fd_table_t table;
    struct vfs_mount mnt = {0};
    int fd;

    stdio_allocations();
    process_allocations();
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
    reader->vnode = &old_node;
    reader->fhandle = &old_node;
    fd_ofd_repoint(&mnt, &old_node, &new_node);
    check(reader->vnode == &new_node && reader->fhandle == &old_node,
          "mounted open handle remains stable during node relocation");
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
        printf("fd_hosttest: pass: stdio allocation and description ordering\n");
    }
    return fails != 0;
}
