/**
 * Drive exit cleanup through a contended close and working-directory release.
 *
 * host-deps:
 *
 * The file callbacks wait on a queue of the kernel's wait queue code, which
 * is included whole. The scheduler substitute releases the lock only after
 * observing the blocked waiter, then wakes it. No timer or probability
 * decides whether cleanup sleeps.
 *
 * The thread's address space must go at its end, once, after its futex words,
 * which are in it, and before its files and the publication of its death, as
 * Linux's exit_mm drops it before the zombie can be reaped, and the processor
 * must go on in the kernel's directory.
 */
#define WAIT_QUEUE_HOSTTEST
#include "proc/wait_queue.c"
#include "proc/exit.c"
#include "arch/i686/mmu.h"
#include "ns_first.h"
#include "proc/tgroup.h"

extern int printf(const char *format, ...);

static process_t child;
static process_t parent;
static struct tgroup child_group;
static struct tgroup parent_group;
struct percpu cpu_table[NR_CPUS];
uint32_t proc_dead_threads;
page_dir_t *_kernel_dir;
static page_dir_t kernel_dir;
static page_dir_t *loaded;
static int dropped;
static wait_queue_t lock_wait;
static int lock_free;
/* Whether the close of the files, and the release of the directories,
 * wait for the lock, and whether the child has a parent to notify. */
static int fd_wait;
static int cwd_wait;
static int with_parent;
static int closed;
static int cwd_released;
static int root_released;
static int adopted;
static int notified;
static int switches;
static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                     \
    do {                                             \
        if (!(condition)) {                          \
            printf("exit-test: fail: %s\n", detail); \
            failures++;                              \
        }                                            \
    } while (0)

/**
 * The condition of the lock wait: whether the lock is free. The process must
 * not be a zombie yet.
 */
static int available(void *arg)
{
    CHECK(arg == &lock_wait, "the wait keeps its lock");
    CHECK(child.state != PROC_ZOMBIE, "cleanup is not yet reapable");
    return lock_free;
}

/**
 * The one switch of a lock wait: check that the child is blocked on the lock,
 * release the lock and wake the child, and return to it running.
 */
void schedule(void)
{
    CHECK(child.state == PROC_BLOCKED, "cleanup blocked before scheduling");
    CHECK(lock_wait.head == &child.waiter, "the lock has the dying waiter");
    CHECK(!notified, "the parent is not notified during cleanup");
    CHECK(child.in_syscall, "interrupt preemption stays deferred");
    switches++;
    lock_free = 1;
    wq_wake_all(&lock_wait);
    CHECK(child.state == PROC_READY, "the lock release wakes cleanup");
    child.state = PROC_RUNNING;
}

/** A wake makes the process ready; there is one processor and no queue. */
void sched_make_ready(process_t *p)
{
    p->state = PROC_READY;
}

/** Exit cleanup must not ask; a call fails the row. */
int signal_pending(void)
{
    CHECK(0, "cleanup must use an uninterruptible wait");
    return 1;
}

/** Take the lock after a wait, which `schedule` ends. */
static void wait_for_lock(void)
{
    lock_free = 0;
    CHECK(wq_wait_event_nointr(&lock_wait, available, &lock_wait) == 0, "the lock wait completes");
    CHECK(child.state == PROC_RUNNING, "the wait returns running");
    CHECK(!wq_active(&lock_wait), "the wait leaves no queued record");
}

/** Close the child's files, after a wait for the lock when `fd_wait` is set. */
void proc_files_put(process_t *p)
{
    CHECK(p == &child, "cleanup closes the dying process's files");
    CHECK(child.state != PROC_ZOMBIE, "files close before death is published");
    if (fd_wait) {
        wait_for_lock();
    }
    closed = 1;
}

/** The working directory and the root go together, with the record they
 * are kept in. */
void proc_fs_put(process_t *p)
{
    CHECK(p == &child && closed, "the directories are released after the files");
    CHECK(child.state != PROC_ZOMBIE, "the directories are released before death is published");
    if (cwd_wait) {
        wait_for_lock();
    }
    cwd_released = root_released = 1;
}

static int vfork_released;
static int thread_released;

/** No row traces the child or has it trace another. */
void ptrace_exit_notify(process_t *p)
{
    (void)p;
    CHECK(0, "an untraced thread tells no tracer");
}

/** Check that the POSIX timers deleted are the child's process's. */
void ptimer_delete_all(struct tgroup *tg)
{
    CHECK(tg == child.tg, "the process's POSIX timers are deleted at its end");
}

/** Check that the semaphore adjustments applied are the child's process's. */
void sysv_exit(struct tgroup *tg)
{
    CHECK(tg == child.tg, "the process's semaphore adjustments are applied at its end");
}

/** Check that the tracees let go are the child's process's. */
void ptrace_tracer_gone(uint32_t tgid)
{
    CHECK(tgid == child.tgid, "the process's own tracees are let go at its end");
}

/**
 * Count the release of the thread's futexes, which comes before its address
 * space and its files go.
 */
void proc_thread_gone(process_t *p)
{
    CHECK(p == &child && !closed && !dropped,
          "the thread's futexes go before its address space and its files");
    thread_released++;
}

/**
 * Count the drop of the address space, which comes after the futexes and before
 * the files.
 */
void mm_drop(struct process *p)
{
    CHECK(p == &child && thread_released == 1 && !closed,
          "the address space goes after the futex words and before the files");
    CHECK(child.state != PROC_ZOMBIE, "the address space goes before death is published");
    dropped++;
}

/** Load `dir`, which must be the kernel's, once the address space is gone. */
void switch_page_directory(page_dir_t *dir)
{
    CHECK(dir == _kernel_dir && dropped == 1,
          "the processor goes on in the kernel's directory once the address space is gone");
    loaded = dir;
}

/**
 * Count the release of a vfork parent, which comes after the files and the
 * directories.
 */
void proc_vfork_release(process_t *p)
{
    CHECK(p == &child && closed && cwd_released, "a vfork parent runs after the files go");
    vfork_released++;
}

/** Note the adoption of the children, which comes after the cleanup. */
void reparent_children(uint32_t pid)
{
    CHECK(pid == child.pid && closed && cwd_released, "children are adopted after cleanup");
    adopted = 1;
}

/** The child's parent, or NULL when the row has none. */
process_t *get_process(uint32_t pid)
{
    CHECK(pid == child.parent_pid, "notification finds the parent");
    return with_parent ? &parent : 0;
}

/** The model's siginfo of a child's end is not read. */
void proc_child_info(const process_t *p, struct k_siginfo *info)
{
    (void)p;
    (void)info;
}

/**
 * Count the SIGCHLD to the parent, which must follow every cleanup and find the
 * child a zombie.
 */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)info;
    CHECK(target == &parent && signo == SIGCHLD, "the parent receives sigchld");
    CHECK(closed && cwd_released && root_released && adopted, "notification follows all cleanup");
    CHECK(child.state == PROC_ZOMBIE, "notification exposes a completed zombie");
    notified++;
}

/**
 * End the child with `status`, waiting for the lock in the close of the files
 * when `files` is set and in the release of the directories when `directory`
 * is, with a parent when `has_parent` is, and check the order and the count of
 * each step.
 */
static void run_case(int files, int directory, int has_parent, int32_t status)
{
    fd_wait = files;
    cwd_wait = directory;
    with_parent = has_parent;
    closed = cwd_released = root_released = adopted = notified = switches = vfork_released = 0;
    thread_released = dropped = 0;
    loaded = 0;
    _kernel_dir = &kernel_dir;
    child.pid = 42;
    child.tgid = 42;
    child.tg = &child_group;
    child_group.leader = &child;
    child_group.live = 1;
    child_group.exiting = 0;
    parent.tg = &parent_group;
    child.parent_pid = 7;
    child.state = PROC_RUNNING;
    child.in_syscall = 0;
    child.exit_code = -1;
    child.waiter.proc = &child;
    child.waiter.next = 0;
    parent.state = PROC_BLOCKED;
    current_proc = &child;
    wait_queue_init(&lock_wait);

    CHECK(proc_exit_cleanup(status) == 1, "the only thread's end is the process's");

    CHECK(switches == files + directory, "every requested wait was reached");
    CHECK(thread_released == 1, "the thread's futexes are released once");
    CHECK(dropped == 1 && loaded == _kernel_dir,
          "the address space goes once, at the thread's end");
    CHECK(child_group.live == 0, "the process has no thread left");
    CHECK(child.state == PROC_ZOMBIE, "cleanup ends as a zombie");
    CHECK(child.exit_code == status, "the encoded wait status is preserved");
    CHECK(child.in_syscall, "remaining teardown cannot be preempted");
    CHECK(notified == has_parent, "the parent is notified once");
    CHECK(vfork_released == 1, "a vfork parent is let go once");
    CHECK(parent.state == (has_parent ? PROC_READY : PROC_BLOCKED),
          "a blocked parent becomes ready");
    CHECK(!wq_active(&lock_wait), "cleanup leaves no waiter");
}

/** Run the five cases; 0 when every check passed, 1 otherwise. */
int main(void)
{
    run_case(0, 0, 1, 7 << 8);
    run_case(1, 0, 1, SIGKILL);
    run_case(0, 1, 1, SIGSEGV);
    run_case(1, 1, 1, 0);
    run_case(1, 1, 0, SIGKILL);
    if (!failures) {
        printf("exit-test: result: five cases passed, six waits completed\n");
    }
    return failures ? 1 : 0;
}
