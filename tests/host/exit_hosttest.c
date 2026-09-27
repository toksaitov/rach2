/**
 * Drive exit cleanup through a contended close and working-directory release.
 *
 * host-deps:
 *
 * The file callbacks wait on the real queue. The scheduler substitute releases
 * the lock only after observing the blocked waiter, then wakes it. No timer or
 * probability decides whether cleanup sleeps.
 */
#define WAIT_QUEUE_HOSTTEST
#include "proc/wait_queue.c"
#include "proc/exit.c"

extern int printf(const char *format, ...);

static process_t child;
static process_t parent;
process_t *current_proc;
static wait_queue_t lock_wait;
static int lock_free;
static int fd_wait;
static int cwd_wait;
static int with_parent;
static int closed;
static int cwd_released;
static int adopted;
static int notified;
static int switches;
static int failures;

#define CHECK(condition, detail)                     \
    do {                                             \
        if (!(condition)) {                          \
            printf("exit-test: fail: %s\n", detail); \
            failures++;                              \
        }                                            \
    } while (0)

static int available(void *arg)
{
    CHECK(arg == &lock_wait, "the wait keeps its lock");
    CHECK(child.state != PROC_ZOMBIE, "cleanup is not yet reapable");
    return lock_free;
}

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

int signal_pending(void)
{
    CHECK(0, "cleanup must use an uninterruptible wait");
    return 1;
}

static void wait_for_lock(void)
{
    lock_free = 0;
    CHECK(wq_wait_event_nointr(&lock_wait, available, &lock_wait) == 0, "the lock wait completes");
    CHECK(child.state == PROC_RUNNING, "the wait returns running");
    CHECK(!wq_active(&lock_wait), "the wait leaves no queued record");
}

void release_fd_table(fd_table_t *table)
{
    CHECK(table == &child.fds, "cleanup closes the dying process's files");
    CHECK(child.state != PROC_ZOMBIE, "files close before death is published");
    if (fd_wait) {
        wait_for_lock();
    }
    closed = 1;
}

void vfs_cwd_clear(process_t *p)
{
    CHECK(p == &child && closed, "the directory is released after the files");
    CHECK(child.state != PROC_ZOMBIE, "the directory is released before death is published");
    if (cwd_wait) {
        wait_for_lock();
    }
    cwd_released = 1;
}

static int vfork_released;

void proc_vfork_release(process_t *p)
{
    CHECK(p == &child && closed && cwd_released, "a vfork parent runs after the files go");
    vfork_released++;
}

void reparent_children(uint32_t pid)
{
    CHECK(pid == child.pid && closed && cwd_released, "children are adopted after cleanup");
    adopted = 1;
}

process_t *get_process(uint32_t pid)
{
    CHECK(pid == child.parent_pid, "notification finds the parent");
    return with_parent ? &parent : 0;
}

void proc_child_info(const process_t *p, struct k_siginfo *info)
{
    (void)p;
    (void)info;
}

void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)info;
    CHECK(target == &parent && signo == SIGCHLD, "the parent receives sigchld");
    CHECK(closed && cwd_released && adopted, "notification follows all cleanup");
    CHECK(child.state == PROC_ZOMBIE, "notification exposes a completed zombie");
    notified++;
}

static void run_case(int files, int directory, int has_parent, int32_t status)
{
    fd_wait = files;
    cwd_wait = directory;
    with_parent = has_parent;
    closed = cwd_released = adopted = notified = switches = vfork_released = 0;
    child.pid = 42;
    child.parent_pid = 7;
    child.state = PROC_RUNNING;
    child.in_syscall = 0;
    child.exit_code = -1;
    child.waiter.proc = &child;
    child.waiter.next = 0;
    parent.state = PROC_BLOCKED;
    current_proc = &child;
    wait_queue_init(&lock_wait);

    proc_exit_cleanup(status);

    CHECK(switches == files + directory, "every requested wait was reached");
    CHECK(child.state == PROC_ZOMBIE, "cleanup ends as a zombie");
    CHECK(child.exit_code == status, "the encoded wait status is preserved");
    CHECK(child.in_syscall, "remaining teardown cannot be preempted");
    CHECK(notified == has_parent, "the parent is notified once");
    CHECK(vfork_released == 1, "a vfork parent is let go once");
    CHECK(parent.state == (has_parent ? PROC_READY : PROC_BLOCKED),
          "a blocked parent becomes ready");
    CHECK(!wq_active(&lock_wait), "cleanup leaves no waiter");
}

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
