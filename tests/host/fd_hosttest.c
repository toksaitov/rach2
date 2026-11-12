/**
 * fd_hosttest.c: the descriptor tables and open file descriptions of
 * rach2/sys/fs/fd.c, and the process allocation of rach2/sys/proc/process.c
 * that makes a table, with the boot's numbering that leaves pid 1 to init.
 *
 * host-deps: tests/host/compat/vfs_name_none.c
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/bkl_none.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The heap counts the allocations live in `allocations`; a row that sets
 * `fail_after` to N lets N more succeed and fails every one after. The
 * namespace lock of the model completes a pending move of a node, as a
 * writer that held the lock would, when a description's owner takes it. A
 * handle read before that wait would still name the old node, though
 * `fd_ofd_repoint` has updated the description.
 */
#include "fs/vfs.h"
#include "compat/waketime_none.h"
#include "lib/kerrno.h"
#include "mm/kheap.h"
#include "proc/process.c"
#include "ns_first.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern void exit(int status);
volatile uint32_t init_pid;
volatile uint64_t kernel_ticks;

/** The console's count of vhangups: none in the model. */
uint32_t tty_console_vhangups(void)
{
    return 0;
}

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

const struct vfs_type vfs_krffs_type = {0, 0, 0, 1, 0, 0};
static const struct vfs_type other_type = {0, 0, 0, 1, 0, 0};
static const struct vfs_type plain_type = {0, 0, 0, 0, 0, 0};
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

/** Zeroed host memory, counted in `allocations`, or NULL once `fail_after`
 *  has counted down to 0. */
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

/** Free what `kzalloc` gave, and count it. */
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

/** The tick's wake of a due sleeper, which no row reaches. */
void sched_wake_due(process_t *p, uint64_t now)
{
    (void)p;
    (void)now;
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

/** These process fixtures have no seccomp filter. */
void seccomp_release(process_t *p)
{
    (void)p;
}

/** No row reparents a child, so a signal fails the test. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    (void)signo;
    check(0, "unexpected signal");
}

/** `signal_post`, without the information. */
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

/** Restore nothing: `kirq_save` masked nothing. */
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

/** No row switches to a process, so the check of a context passes. */
int arch_context_check(void)
{
    return 0;
}

/** No row takes a lock. */
int lock_any(void)
{
    return 0;
}

/** No description has a lock to drop. */
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

/** End the test with a failure: no row reaches a panic. */
void k_panic(const char *message)
{
    printf("fd_hosttest: fail: unexpected panic: %s\n", message);
    exit(1);
}

/** Make `wq` empty; no process sleeps on it. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = 0;
}

/**
 * The wait for a busy description: check that it starts busy with no
 * namespace lock held, end the other operation that holds it, as that
 * operation would while the caller sleeps, and count the wait. Returns 0.
 */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    check(!cond(arg), "description wait starts busy");
    check(depth == 0, "the description wait holds no namespace lock");
    ((struct open_file *)arg)->busy = 0;
    check(cond(arg), "description wait ends idle");
    waits++;
    return 0;
}

/** Check that a wake follows the description's release, once the rows of
 * the description have one; a process freed before them wakes its exit
 * waiters with none open. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    check(!reader || !reader->busy, "wake follows description release");
}

/**
 * Take the namespace lock, counted in `depth`. A move of the node that is
 * pending is completed first with `fd_ofd_repoint`, as another writer that
 * held the lock would complete it, and counted as a wait.
 */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    if (pending) {
        check(reader->busy, "the description lock precedes the namespace lock");
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
        check(reader->busy, "the namespace lock is let go before the description lock");
    }
    depth--;
}

/* The description a row has its file system close, with its serial, and
 * whether `fd_ofd_live` called it open meanwhile. */
static const struct open_file *closing;
static uint32_t closing_serial;
static int closing_live = -1;

/** A file system's close, which has nothing to release. For the
 *  description `closing` names, it records in `closing_live` whether
 *  `fd_ofd_live` finds it open meanwhile. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    (void)node;
    (void)handle;
    if (closing) {
        closing_live = fd_ofd_live(closing, closing_serial);
    }
}

/** An operation of the root's adapter, which takes `handle` by value and
 *  the namespace lock again, recursively: the handle and the description's
 *  node must both be the node after its move. */
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

    check(fd_init_stdio(0, 0) == -1, "stdio rejects a missing table");
    check(fd_init_stdio(&table, 0) == -1, "stdio rejects an unallocated table");
    /* Each description and its count record. */
    for (fail = 0; fail < 6; fail++) {
        check(fd_table_init(&table) == 0, "stdio table allocation");
        fail_after = fail;
        check(fd_init_stdio(&table, 0) == -1, "stdio reports allocation failure");
        fail_after = -1;
        check(allocations == 1, "stdio releases partial allocations");
        for (i = FD_STDIN; i <= FD_STDERR; i++) {
            fd_entry_t *e = fd_slot(&table, i);

            check(e->type == FD_TYPE_NONE && !e->ofd && !e->flags,
                  "stdio failure leaves an empty table");
            fd_free(&table, i);
        }
        check(fd_init_stdio(&table, 0) == 0, "stdio retry succeeds");
        check(allocations == 7, "stdio allocates three descriptions and their count records");
        for (i = FD_STDIN; i <= FD_STDERR; i++) {
            fd_entry_t *e = fd_slot(&table, i);

            check(e->type == FD_TYPE_CONSOLE && e->ofd, "stdio descriptor has a description");
            check(e->flags == (FD_FLAG_READ | FD_FLAG_WRITE),
                  "each stdio descriptor is open for reading and writing");
            if (!e->ofd) {
                continue;
            }
            check(e->ofd->refcount == 1 && !e->ofd->offset && !e->ofd->raw_offset &&
                      !e->ofd->status_flags,
                  "stdio description starts cleared");
            for (j = FD_STDIN; j < i; j++) {
                check(e->ofd != fd_slot(&table, j)->ofd, "stdio descriptions are separate");
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

/**
 * The boot's numbering: a kernel thread made before init, pid 1 as the DRM
 * commit worker takes it when a display driver binds, moves to the next
 * free pid when the numbering starts again, so that init is pid 1, which
 * BusyBox's reboot signals; a record with an address space at pid 1 keeps
 * it.
 */
static void boot_pids(void)
{
    process_t *idle = alloc_process();
    process_t *worker = alloc_process();
    process_t *init;
    uint32_t moved;

    check(idle && worker && idle->pid == 0 && worker->pid == 1,
          "the boot's first records take pids 0 and 1");
    if (!idle || !worker) {
        return;
    }
    proc_reclaim_boot_pids();
    moved = worker->pid;
    init = alloc_process();
    check(init && init->pid == 1 && get_process(1) == init,
          "init is pid 1 after the numbering starts again");
    check(moved > 1 && worker->tgid == moved && worker->pgid == moved && worker->sid == moved &&
              get_process(moved) == worker,
          "the kernel thread that held pid 1 has the next free one");
    if (init) {
        init->space_root = 1;
        proc_reclaim_boot_pids();
        check(init->pid == 1 && get_process(1) == init,
              "a record with an address space keeps pid 1");
        init->space_root = 0;
        close_process(init);
    }
    close_process(worker);
    close_process(idle);
}

/** Fail each process allocation before the record can enter the live list. */
static void process_allocations(void)
{
    process_t *keeper = alloc_process();
    int fail, held = allocations;
    uint32_t forks = process_forks_since_boot();

    check(keeper != NULL, "initial process allocation");
    for (fail = 0; fail < 12; fail++) {
        process_t *p;

        fail_after = fail;
        p = alloc_process();
        fail_after = -1;
        check(!p, "process reports standard-description allocation failure");
        check(proc_list == keeper && proc_tail == keeper && !keeper->next,
              "failed process is not published");
        check(process_forks_since_boot() == forks, "failed process is not counted");
        check(allocations == held, "failed process releases every allocation");
        if (p) {
            close_process(p);
            forks = process_forks_since_boot();
        }
    }
    close_process(keeper);
    /* The pid index lives as long as the kernel: the rows after this one
     * count from it. */
    check(!proc_list && !proc_tail && allocations == 1, "process cleanup releases all storage");
    allocations = 0;
}

/** A thread allocation shares the records it keeps and makes no stdio.
 * Failures before publication leave the parent's references and live-thread
 * count unchanged; freeing an unfinished child releases only its reference. */
static void child_allocations(void)
{
    process_t *parent = alloc_process();
    process_t *child;
    int before = allocations, fail;
    uint32_t forks = process_forks_since_boot();

    check(parent != NULL, "parent for child allocation");
    if (!parent) {
        return;
    }
    for (fail = 0; fail < 2; fail++) {
        fail_after = fail;
        child = alloc_process_fork(parent, 1, 1);
        fail_after = -1;
        check(!child && allocations == before, "failed child frees its allocations");
        check(parent->tg->refs == 1 && parent->tg->live == 1 && parent->fs->refs == 1,
              "failed child keeps parent reference counts");
        check(process_forks_since_boot() == forks, "failed child is not counted");
    }
    child = alloc_process_fork(parent, 1, 1);
    check(child != NULL, "child allocation");
    if (child) {
        check(child->state == PROC_UNUSED && !child->fds, "child starts unfinished without stdio");
        check(child->tg == parent->tg && child->tgid == parent->tgid && child->fs == parent->fs,
              "thread directly shares its group and directories");
        check(allocations == before + 2 && parent->tg->refs == 2 && parent->tg->live == 1,
              "only the record and floating-point area are allocated");
        free_process(child);
        check(allocations == before && parent->tg->refs == 1 && parent->fs->refs == 1,
              "unfinished thread releases its references");
    }
    close_process(parent);
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
    second->fds = 0;
    check(fd_table_share(first->fds, second) == 0 && second->fds == first->fds,
          "the table is shared");
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
    free_process(second);
    check(first->fds->refs == 1 && first->fds->sharers == first && !first->fd_sharer_next,
          "a sharer that goes leaves the table and its list of sharers");
    close_process(first);
    check(!proc_list && !allocations, "the shared table's cleanup releases all storage");
}

/**
 * A descriptor of a shared table: a reservation is no descriptor to a
 * lookup, a copy, a listing or a close until it is published; a close of a
 * descriptor that another sharer's call holds only marks it closing, which
 * no lookup or second close passes, and the end of the holder's call claims
 * its release, as Linux's last `fput` releases a file another thread's call
 * kept.
 */
static void shared_closes(void)
{
    process_t *first = alloc_process();
    process_t *second = alloc_process();
    struct open_file *ofd;
    fd_entry_t copy;
    int i, fd;

    check(first && second, "two processes for the closes");
    if (!first || !second) {
        return;
    }
    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        fd_free(second->fds, i);
    }
    fd_table_delete(second->fds);
    second->fds = 0;
    check(fd_table_share(first->fds, second) == 0, "the table is shared for the closes");
    current_proc = second;
    fd = fd_alloc(second->fds);
    check(fd == 3, "the second sharer reserves descriptor 3");
    if (fd != 3) {
        return;
    }
    fd_slot(second->fds, fd)->type = FD_TYPE_CONSOLE;
    ofd = fd_slot(second->fds, fd)->ofd;
    current_proc = first;
    check(!fd_get(first->fds, fd) && fd_peek(first->fds, fd, &copy, 0) == -EBADF &&
              fd_next_open(first->fds, fd) == -1 && fd_close_claim(first->fds, fd) == -EBADF,
          "a reservation is no descriptor until it is published");
    fd_install(second->fds, fd);
    check(fd_peek(first->fds, fd, &copy, FD_PEEK_REF) == 0 && copy.ofd == ofd &&
              ofd->refcount == 2 && fd_next_open(first->fds, 0) == FD_STDIN &&
              fd_next_open(first->fds, fd) == fd,
          "a published descriptor is found, and a copy holds its description");
    fd_ofd_unref(copy.ofd);
    check(fd_close_claim(first->fds, fd) == 0 &&
              (fd_slot(first->fds, fd)->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) ==
                  FD_FLAG_CLOSING,
          "a close of a descriptor another sharer's call holds marks it closing");
    check(!fd_get(first->fds, fd) && fd_close_claim(first->fds, fd) == -EBADF &&
              fd_peek(first->fds, fd, &copy, 0) == -EBADF,
          "no lookup, copy or second close passes a closing descriptor");
    current_proc = second;
    check(fd_holds_claim(0) == fd && (fd_slot(second->fds, fd)->flags & FD_FLAG_RELEASING),
          "the end of the holder's call claims the release");
    check(fd_holds_claim(fd + 1) == -1 && second->fd_holding == 0,
          "and the call holds nothing more");
    check(ofd->refcount == 1 && fd_ofd_unref_unless_last(ofd) == 0,
          "the claimed descriptor holds the description's last reference");
    fd_ofd_unref(ofd);
    fd_free_after(second->fds, fd);
    current_proc = first;
    check(fd_holds_claim(0) == -1 && fd_alloc(first->fds) == fd,
          "a released slot is free for the next open");
    current_proc = 0;
    fd_free(first->fds, fd);
    check(!first->fds->lock.locked, "every call lets the table's lock go");
    free_process(second);
    close_process(first);
    check(!proc_list && !allocations, "the closes leave nothing allocated");
}

/**
 * A close that left a descriptor to another sharer's call takes its release
 * over once that hold goes without the call's end, as a close does after it
 * lets an epoll_wait asleep go of the descriptor: never while the hold
 * stays, and once alone.
 */
static void close_finish(void)
{
    process_t *first = alloc_process();
    process_t *second = alloc_process();
    struct open_file *ofd;
    int i, fd;

    check(first && second, "two processes for the finished close");
    if (!first || !second) {
        return;
    }
    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        fd_free(second->fds, i);
    }
    fd_table_delete(second->fds);
    second->fds = 0;
    check(fd_table_share(first->fds, second) == 0, "the table is shared for the finished close");
    current_proc = second;
    fd = fd_alloc(second->fds);
    check(fd == 3, "the second sharer holds descriptor 3");
    if (fd != 3) {
        return;
    }
    fd_slot(second->fds, fd)->type = FD_TYPE_CONSOLE;
    ofd = fd_slot(second->fds, fd)->ofd;
    fd_install(second->fds, fd);
    current_proc = first;
    check(fd_close_finish(first->fds, fd) == 0, "an open descriptor has no close to finish");
    check(fd_close_claim(first->fds, fd) == 0 && fd_close_finish(first->fds, fd) == 0,
          "a close does not finish while another sharer's call holds the descriptor");
    second->fd_held[0] &= ~(1u << fd);
    check(fd_close_finish(first->fds, fd) == 1 &&
              (fd_slot(first->fds, fd)->flags & FD_FLAG_RELEASING),
          "once the hold goes, the close takes the release over");
    check(fd_close_finish(first->fds, fd) == 0, "and takes it once");
    current_proc = second;
    check(fd_holds_claim(0) == -1 && second->fd_holding == 0,
          "the end of the call that lost its hold releases nothing");
    current_proc = first;
    check(ofd->refcount == 1 && fd_ofd_unref_unless_last(ofd) == 0,
          "the close holds the description's last reference");
    fd_ofd_unref(ofd);
    fd_free_after(first->fds, fd);
    current_proc = 0;
    check(!first->fds->lock.locked, "the finished close lets the table's lock go");
    free_process(second);
    close_process(first);
    check(!proc_list && !allocations, "the finished close leaves nothing allocated");
}

/**
 * A growth adds a chunk as large as the table and moves no entry, so the
 * address of an entry stays its slot's: chunk k > 0 holds the entries from
 * FD_TABLE_FIRST << (k - 1), each chunk contiguous.
 */
static void table_chunks(void)
{
    fd_table_t table;
    fd_entry_t *first;
    int fd, k;

    check(fd_table_init(&table) == 0, "a table for the chunks");
    fd = fd_alloc(&table);
    check(fd == 0, "the first descriptor");
    if (fd != 0) {
        return;
    }
    fd_slot(&table, fd)->type = FD_TYPE_CONSOLE;
    fd_install(&table, fd);
    first = fd_slot(&table, fd);
    check(fd_table_reach(&table, 100) == 0 && fd_table_cap(&table) == 128,
          "a reach of 100 grows the table to 128");
    check(fd_slot(&table, 0) == first && fd_get(&table, 0) == first,
          "a growth leaves an entry where it was");
    for (k = 1; k < 4; k++) {
        int start = FD_TABLE_FIRST << (k - 1);

        check(fd_slot(&table, 2 * start - 1) == fd_slot(&table, start) + start - 1 &&
                  table.chunks[k] == fd_slot(&table, start),
              "each chunk holds as many entries as those before it, in order");
    }
    check(!table.chunks[4] && !table.lock.locked, "no chunk past the last, and the lock is free");
    fd_free(&table, fd);
    fd_table_free(&table);
    check(allocations == 0, "the chunks leave nothing allocated");
}

/**
 * Which descriptor of a description is the last is decided in one step on
 * the count: a reference that is not the last is dropped, and the last is
 * left for its holder to release the description's objects and then drop.
 */
static void description_last(void)
{
    struct open_file *ofd = fd_ofd_new();

    check(ofd != 0, "a description for the last reference");
    if (!ofd) {
        return;
    }
    fd_ofd_ref(ofd);
    check(fd_ofd_unref_unless_last(ofd) == 1 && ofd->refcount == 1,
          "a reference that is not the last is dropped");
    check(fd_ofd_unref_unless_last(ofd) == 0 && ofd->refcount == 1,
          "the last is left for its holder");
    fd_ofd_unref(ofd);
    check(allocations == 0, "the last reference frees the description");
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
    fd_slot(&table, open)->type = FD_TYPE_CONSOLE;
    fd_install(&table, open);
    copy = fd_table_dup(&table);
    check(copy != NULL, "table copy allocation");
    if (copy) {
        check(fd_slot(copy, reserved)->type == FD_TYPE_NONE && !fd_slot(copy, reserved)->ofd,
              "a reserved slot is free in the copy");
        check(fd_slot(copy, open)->type == FD_TYPE_CONSOLE &&
                  fd_slot(copy, open)->ofd == fd_slot(&table, open)->ofd,
              "an open descriptor is in the copy");
        check(fd_alloc(copy) == reserved, "the copy allocates the reserved slot");
        fd_free(copy, reserved);
        k_memset(fd_slot(copy, open), 0, sizeof((*fd_slot(copy, open))));
        fd_table_delete(copy);
    }
    fd_free(&table, reserved);
    fd_free(&table, open);
    fd_table_free(&table);
    check(allocations == 0, "table copy releases every allocation");
}

/**
 * A descriptor the heap has no room for is ENOMEM, whether its description
 * or the table's growth fails, and leaves its slot free; a minimum at the
 * limit is EMFILE. The growth a dup2 needs is ENOMEM when the heap has no
 * room, and a destination outside the limit is EBADF.
 */
static void allocation_errors(void)
{
    fd_table_t table;
    int i, fd;

    check(fd_table_init(&table) == 0, "a table for the allocation errors");
    fail_after = 0;
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && allocations == 1 && fd_slot(&table, 0)->type == FD_TYPE_NONE &&
              !fd_slot(&table, 0)->ofd,
          "a description the heap has no room for is ENOMEM and leaves the slot free");
    for (i = 0; i < FD_TABLE_FIRST; i++) {
        check(fd_alloc(&table) == i, "the first entries are taken in order");
    }
    fail_after = 0;
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == FD_TABLE_FIRST,
          "a description the heap has no room for in a full table is ENOMEM");
    i = allocations;
    fail_after = 2; /* the description's record and its spare count */
    fd = fd_alloc(&table);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == FD_TABLE_FIRST && allocations == i,
          "a table the heap has no room to grow is ENOMEM and frees the description");
    check(fd_alloc(&table) == FD_TABLE_FIRST && fd_table_cap(&table) == 2 * FD_TABLE_FIRST,
          "a growth adds a chunk as large as the table");
    check(fd_alloc_from(&table, fd_nr_open) == -EMFILE, "a minimum at the limit is EMFILE");
    fail_after = 0;
    fd = fd_table_reach(&table, FD_TABLE_LIMIT - 1);
    fail_after = -1;
    check(fd == -ENOMEM && fd_table_cap(&table) == 2 * FD_TABLE_FIRST,
          "a dup2 past the end of a table the heap cannot grow is ENOMEM");
    check(fd_table_reach(&table, fd_nr_open) == -EBADF && fd_table_reach(&table, -1) == -EBADF,
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
 * Whether a description is open still, which an epoll registration checks
 * of the one it was made for: while a reference is left, by the serial it was
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

/** A mount is its own base: no row makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/**
 * Run the rows of the functions above, then those of the description lock
 * and the namespace lock on one description:
 *
 * - on the KRFFS root's type the description lock comes first, as Linux's
 *   f_pos_lock comes before i_rwsem, and the namespace lock after it
 *   completes a pending move, which updates the description's node and
 *   handle, so an operation that reads the handle after the locks gets the
 *   moved node; the namespace lock is let go before the description lock;
 * - a description another operation holds is waited for with no namespace
 *   lock held, and a wake follows its release;
 * - on another type whose descriptions take the namespace lock, a move
 *   updates the node and leaves the handle, an open record of the type's
 *   own;
 * - a type that needs no serialising takes no namespace lock, and a
 *   description with no mount takes only its own lock.
 *
 * Returns 0 when every check passed and 1 otherwise.
 */
int main(void)
{
    fd_table_t table;
    struct vfs_mount mnt = {0};
    int fd;

    stdio_allocations();
    boot_pids();
    process_allocations();
    child_allocations();
    shared_limits();
    table_copies();
    allocation_errors();
    description_holders();
    description_live();
    shared_closes();
    close_finish();
    table_chunks();
    description_last();
    if (fd_table_init(&table) != 0 || (fd = fd_alloc(&table)) < 0) {
        return 1;
    }
    reader = fd_slot(&table, fd)->ofd;
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
    check(depth == 1, "other mounted types take the namespace lock after the description lock");
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
               "shared closes, chunks, allocation errors and open descriptions\n");
    }
    return fails != 0;
}
