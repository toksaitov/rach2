/**
 * Check standard description allocation, locking across relocation, the
 * descriptor limit of each process that shares a table, the errors of an
 * allocation the limit or the heap refuses, and whether a description is
 * open still.
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
#include "lib/kerrno.h"
#include "mm/kheap.h"
#include "proc/process.c"
#include "ns_first.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern void exit(int status);
volatile uint32_t init_pid;
volatile uint32_t kernel_ticks;

const struct vfs_type vfs_krffs_type = {0, 0, 0, 1, 0};
static const struct vfs_type other_type = {0, 0, 0, 1, 0};
static const struct vfs_type plain_type = {0, 0, 0, 0, 0};
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

/** Supply the process allocator's floating-point area from the same
 * counted allocations. */
void *fpu_area_alloc(void)
{
    return kzalloc(512);
}

/** Release the area `fpu_area_alloc` gave. */
void fpu_area_free(void *area)
{
    kfree(area);
}

/** A wake makes the process ready; there is one processor and no queue. */
void sched_make_ready(process_t *p)
{
    p->state = PROC_READY;
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

/** These process fixtures copy no path. */
void path_bufs_free(process_t *p)
{
    (void)p;
}

/** Nor have they a seccomp filter. */
void seccomp_release(process_t *p)
{
    (void)p;
}

/** No row reparents a child. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    (void)signo;
    check(0, "unexpected signal");
}

void signal_post_info(process_t *p, int signo, const struct k_siginfo *info)
{
    (void)info;
    signal_post(p, signo);
}

/** The CPU time no row reads is summed without an interrupt to mask. */
uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** No row switches to a process, so the context is never laid out. */
void arch_context_init(struct process *proc, void (*entry)(void))
{
    (void)proc;
    (void)entry;
}

int arch_context_check(void)
{
    return 0;
}

/** No row takes a lock. */
int lock_any(void)
{
    return 0;
}

void lock_ofd_gone(const struct open_file *ofd)
{
    (void)ofd;
}

/** fd.c reports a description's close to inotify, and no row watches. */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
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

/* The description a row has its file system close, with its serial, and
 * whether `fd_ofd_live` called it open meanwhile. */
static const struct open_file *closing;
static uint32_t closing_serial;
static int closing_live = -1;

/** No backing resources exist in this lock test; a row that names the
 * description being closed learns whether it is open meanwhile. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    (void)node;
    (void)handle;
    if (closing) {
        closing_live = fd_ofd_live(closing, closing_serial);
    }
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
        fd_free(p->fds, i);
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
    for (fail = 0; fail < 9; fail++) {
        process_t *p;

        fail_after = fail;
        p = alloc_process();
        fail_after = -1;
        check(!p, "process reports standard-description allocation failure");
        check(proc_list == keeper && proc_tail == keeper && !keeper->next,
              "failed process is not published");
        check(process_forks_since_boot() == forks, "failed process is not counted");
        check(allocations == 9, "failed process releases every allocation");
        if (p) {
            close_process(p);
            forks = process_forks_since_boot();
        }
    }
    close_process(keeper);
    check(!proc_list && !proc_tail && !allocations, "process cleanup releases all storage");
}

/**
 * Two processes that share one table, as clone with CLONE_FILES makes them,
 * the second with a soft RLIMIT_NOFILE of 4: the second is held to 4 when it
 * takes a descriptor, and the first, whose limit is the default, takes one
 * past it, the second's limit not lowering the first's.
 */
static void shared_limits(void)
{
    process_t *first = alloc_process();
    process_t *second = alloc_process();
    int i;

    check(first && second, "two processes for a shared table");
    if (!first || !second) {
        return;
    }
    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        fd_free(second->fds, i);
    }
    fd_table_delete(second->fds);
    check(fd_table_share(first->fds) == 0, "the table is shared");
    second->fds = first->fds;
    second->rlim[RLIM_NOFILE].cur = 4;
    current_proc = second;
    check(fd_limit(second->fds) == 4 && fd_alloc(second->fds) == 3,
          "the process with a limit of 4 takes descriptor 3");
    check(fd_alloc(second->fds) == -EMFILE && !fd_can_alloc(second->fds),
          "the process with a limit of 4 is held to it in the shared table");
    current_proc = first;
    check(fd_limit(first->fds) == FD_TABLE_LIMIT && fd_alloc(first->fds) == 4,
          "a sharer keeps its own limit and takes descriptor 4");
    current_proc = 0;
    fd_free(first->fds, 3);
    fd_free(first->fds, 4);
    first->fds->refs--;
    second->fds = 0;
    free_process(second);
    close_process(first);
    check(!proc_list && !allocations, "the shared table's cleanup releases all storage");
}

/**
 * Copy a table with a slot another thread's open has reserved and one
 * descriptor that is open: the reservation is free in the copy, and the open
 * descriptor is there with the description it shares.
 */
static void table_copies(void)
{
    fd_table_t table;
    fd_table_t *copy;
    int reserved, open;

    check(fd_table_init(&table) == 0, "table allocation for the copy");
    reserved = fd_alloc(&table);
    open = fd_alloc(&table);
    check(reserved >= 0 && open >= 0, "a reservation and an open descriptor");
    if (reserved < 0 || open < 0) {
        return;
    }
    table.entries[open].type = FD_TYPE_CONSOLE;
    copy = fd_table_dup(&table);
    check(copy != NULL, "table copy allocation");
    if (copy) {
        check(copy->entries[reserved].type == FD_TYPE_NONE && !copy->entries[reserved].ofd,
              "a reserved slot is free in the copy");
        check(copy->entries[open].type == FD_TYPE_CONSOLE &&
                  copy->entries[open].ofd == table.entries[open].ofd,
              "an open descriptor is in the copy");
        check(fd_alloc(copy) == reserved, "the copy allocates the reserved slot");
        fd_free(copy, reserved);
        k_memset(&copy->entries[open], 0, sizeof(copy->entries[open]));
        fd_table_delete(copy);
    }
    fd_free(&table, reserved);
    fd_free(&table, open);
    fd_table_free(&table);
    check(allocations == 0, "table copy releases every allocation");
}

/**
 * A descriptor the heap has no room for is ENOMEM, whether its description
 * or the table's growth is refused, and leaves its slot free; a minimum at
 * the limit is EMFILE. The growth dup2 asks for is ENOMEM when the heap has
 * no room, and a destination outside the limit is EBADF.
 */
static void allocation_errors(void)
{
    fd_table_t table;
    int i, fd;

    check(fd_table_init(&table) == 0, "a table for the allocation errors");
    fail_after = 0;
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && allocations == 1 && table.entries[0].type == FD_TYPE_NONE &&
              !table.entries[0].ofd,
          "a description the heap has no room for is ENOMEM and leaves the slot free");
    for (i = 0; i < FD_TABLE_FIRST; i++) {
        check(fd_alloc(&table) == i, "the first entries are taken in order");
    }
    fail_after = 0;
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == FD_TABLE_FIRST,
          "a table the heap has no room to grow is ENOMEM");
    fail_after = 1;
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == 2 * FD_TABLE_FIRST &&
              !table.entries[FD_TABLE_FIRST].ofd && fd_alloc(&table) == FD_TABLE_FIRST,
          "a description the heap has no room for in a grown table is ENOMEM");
    check(fd_alloc_from(&table, FD_TABLE_LIMIT) == -EMFILE, "a minimum at the limit is EMFILE");
    fail_after = 0;
    fd = fd_table_reach(&table, FD_TABLE_LIMIT - 1);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == 2 * FD_TABLE_FIRST,
          "a dup2 past the end of a table the heap cannot grow is ENOMEM");
    check(fd_table_reach(&table, FD_TABLE_LIMIT) == -EBADF && fd_table_reach(&table, -1) == -EBADF,
          "a dup2 destination outside the limit is EBADF");
    for (i = 0; i <= FD_TABLE_FIRST; i++) {
        fd_free(&table, i);
    }
    fd_table_free(&table);
    check(allocations == 0, "the allocation errors leave nothing allocated");
}

/**
 * Every description of a node holds it, one whose release is under way only
 * while another reference keeps it, and a description of another node or
 * one released holds nothing.
 */
static void description_holders(void)
{
    struct vfs_mount mnt = {0};
    struct open_file *a = fd_ofd_new();
    struct open_file *b = fd_ofd_new();
    struct open_file *other = fd_ofd_new();
    int node, elsewhere;

    check(a && b && other, "three descriptions");
    if (!a || !b || !other) {
        return;
    }
    a->vmnt = b->vmnt = other->vmnt = &mnt;
    a->vnode = b->vnode = &node;
    other->vnode = &elsewhere;
    check(fd_ofd_holders(&mnt, &node, NULL) == 2, "every description of the node holds it");
    check(fd_ofd_holders(&mnt, &node, a) == 1, "a description that is going does not");
    fd_ofd_ref(a);
    check(fd_ofd_holders(&mnt, &node, a) == 2, "one that another reference keeps does");
    fd_ofd_unref(a);
    fd_ofd_unref(a);
    fd_ofd_unref(b);
    fd_ofd_unref(other);
    check(fd_ofd_holders(&mnt, &node, NULL) == 0 && allocations == 0,
          "released descriptions hold nothing");
}

/**
 * Whether a description is open still, which an epoll registration asks of
 * the one it was made for: while a reference is left, by the serial it was
 * made with and no other, which a later description at its address has; not
 * while its file system closes it after its last reference, nor after.
 */
static void description_live(void)
{
    struct vfs_mount mnt = {0};
    struct open_file *ofd = fd_ofd_new();
    uint32_t serial;

    check(ofd != 0, "a description");
    if (!ofd) {
        return;
    }
    serial = ofd->serial;
    check(fd_ofd_live(ofd, serial), "a description with a reference is open");
    check(!fd_ofd_live(ofd, serial + 1), "a description is not one with another serial");
    fd_ofd_ref(ofd);
    fd_ofd_unref(ofd);
    check(fd_ofd_live(ofd, serial), "a description a second reference kept is open");
    ofd->vmnt = &mnt;
    closing = ofd;
    closing_serial = serial;
    fd_ofd_unref(ofd);
    closing = 0;
    check(closing_live == 0,
          "a description its file system closes after its last reference is not open");
    check(!fd_ofd_live(ofd, serial) && allocations == 0,
          "a description is not open after its last reference");
}

/** Exercise a waiting independent open, a shared open, and other file types. */
/* A mount is its own base here: no test makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

int main(void)
{
    fd_table_t table;
    struct vfs_mount mnt = {0};
    int fd;

    stdio_allocations();
    process_allocations();
    shared_limits();
    table_copies();
    allocation_errors();
    description_holders();
    description_live();
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
    check(depth == 1, "other mounted types take the namespace before the description");
    fd_ofd_unlock(reader);
    mnt.type = &plain_type;
    fd_ofd_lock(reader);
    check(depth == 0, "a file system that needs no serialising takes no namespace lock");
    fd_ofd_unlock(reader);
    reader->vmnt = 0;
    fd_ofd_lock(reader);
    fd_ofd_unlock(reader);
    check(depth == 0 && !reader->busy, "raw description lock is balanced");
    fd_free(&table, fd);
    fd_table_free(&table);
    if (!fails) {
        printf("fd_hosttest: pass: stdio allocation, description ordering, shared limits, "
               "allocation errors and open descriptions\n");
    }
    return fails != 0;
}
