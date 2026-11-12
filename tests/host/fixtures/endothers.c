/**
 * The end of every other user process before an init that ran under the
 * debug shell hands the machine back to it (`proc_end_others`).
 *
 * The rule of mk/host-fixtures.mk extracts `proc_end_others` unchanged from
 * rach2/sys/syscall_proc.c into endothers.inc, with the `proc_is_kthread` of
 * rach2/sys/proc/process.c that it asks. The records here have the fields
 * they read, and the signal, the scheduler and the reap are models. A switch
 * lets the others run: a process sent SIGKILL ends, and so does one asleep in
 * the call that ends it, as one asleep in a file's release does once the
 * release is done; each is a zombie after the switch. A kernel thread never
 * ends. A walk that goes on switching when nothing is left to end would wait
 * forever, so a sixteenth switch ends the run.
 *
 * A kernel thread, which has no address space, is left alone and not waited
 * for, whether it is in no call or marked in one, as it is while it runs a
 * system call's code: in the first row it is the only other process, and
 * the walk returns without a switch. The second row has a process in the
 * call that ends it, which has given up its address space
 * (`proc_exit_cleanup`): it must be waited for until it is a zombie, then
 * reaped, and sent no signal. Beside it, a record another reaper has taken
 * off the zombies must be left alone and not waited for. The third row has a
 * process that runs in its address space, which must be sent SIGKILL,
 * waited for and reaped. The fourth has such a process running on another
 * processor, which takes its SIGKILL while init waits for an interrupt with
 * the big lock let go (`idle_wait_irq_on`). Each row has both kernel threads.
 *
 * The same rule extracts `exec_alone` and `exec_one_thread`, the end of the
 * other threads of a process before execve replaces its image, as Linux's
 * de_thread ends them (Linux 6.12.115, fs/exec.c:1052-1059, 1080-1085 and
 * 1151-1157). The wait is a model: at each sleep, a thread sent SIGKILL
 * ends unless it is held in a wait no signal ends, and the caller may be
 * sent SIGKILL, alone or as the process. A sixteenth sleep ends the run, as
 * a wait that never ends. The rows: the others end and the exec goes on;
 * SIGKILL for the caller, and SIGKILL for the process, while a thread is
 * held, ends the wait with -EAGAIN and clears `exec_task`; an ending
 * process, and one whose other thread execs already, give -EAGAIN at once,
 * send nothing and leave `exec_task` alone.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGKILL         9
#define SI_KERNEL       0x80
#define KSIG_BIT(signo) (1ULL << ((signo) - 1))
#ifndef EAGAIN
#define EAGAIN 11
#endif

typedef enum { PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE } proc_state_t;

/* A wait queue, which the model's wait only names. */
typedef struct {
    int unused;
} wait_queue_t;

typedef int (*wq_cond_fn)(void *arg);

/* The fields of a thread group the extracted code reads. */
struct tgroup {
    uint32_t live;
    uint64_t pending;
    uint32_t exiting;
    struct process *exec_task;
    wait_queue_t gone;
};

/* The field of a siginfo the extracted code sets. */
struct k_siginfo {
    int32_t code;
};

/* The fields of the kernel's process record the extracted code reads, and
 * the model's own: the signal posted to it, how often it was given back,
 * and whether it is held in a wait no signal ends. */
typedef struct process {
    struct process *next;
    struct tgroup *tg;
    proc_state_t state;
    uint32_t pid;
    uint32_t tgid;
    uintptr_t space_root;
    volatile uint32_t in_syscall;
    void (*kthread_entry)(void);
    uint64_t sig_pending;
    int signalled;
    int released;
    int held;
} process_t;

process_t *proc_list;
process_t *current_proc;
process_t *proc_idle;

static int fails, switches;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("endothers: fail: %s\n", what);
        fails++;
    }
}

/** Post `signo` to `p`, which the model records. */
static void signal_post(process_t *p, int signo)
{
    p->signalled = signo;
}

/** Give back `p` as a reap does: off the list, once it is no zombie. */
static void proc_release(process_t *p)
{
    process_t **link = &proc_list;

    check(p->state == PROC_UNUSED, "a zombie is taken off the zombies before it is given back");
    while (*link && *link != p) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = p->next;
    }
    p->released++;
}

/** Let the others run: a process sent SIGKILL, and one asleep in the call
 * that ends it, end and are zombies; a kernel thread never ends. */
static void schedule(void)
{
    process_t *p;

    if (++switches == 16) {
        printf("endothers: fail: the walk waits for a record that never ends\n");
        exit(1);
    }
    for (p = proc_list; p; p = p->next) {
        if (p != current_proc && !p->kthread_entry &&
            (p->state == PROC_READY || p->state == PROC_BLOCKED) &&
            (p->signalled == SIGKILL || (p->space_root == 0 && p->in_syscall))) {
            p->state = PROC_ZOMBIE;
            p->space_root = 0;
        }
    }
}

/** Wait for an interrupt with the big lock let go: a process sent SIGKILL
 * while it runs on another processor takes the signal there, in an
 * interrupt that needs the lock, and ends. */
static void idle_wait_irq_on(void)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p != current_proc && p->state == PROC_RUNNING && p->signalled == SIGKILL) {
            p->state = PROC_ZOMBIE;
            p->space_root = 0;
        }
    }
}

static struct tgroup execs; /* the exec's process */
static int sleeps;          /* the exec's wait's sleeps */
static int kill_at;         /* the sleep after which SIGKILL comes, or 0 */
static int kill_process;    /* that SIGKILL is sent to the process */

/** Clear `s` as the kernel's k_memset does. */
static void *k_memset(void *s, int c, size_t n)
{
    return memset(s, c, n);
}

/** Set `signo` pending for `t` alone, as the SIGKILL of an exec's end of the
 * other threads is set, and record it. */
static void sig_post_own(process_t *t, int signo, const struct k_siginfo *info)
{
    check(info->code == SI_KERNEL, "the exec's SIGKILL comes from the kernel");
    t->sig_pending |= KSIG_BIT(signo);
    t->signalled = signo;
}

/** Sleep in the exec's wait until `cond` holds: at each sleep, every other
 * thread of `execs` sent SIGKILL and not held ends, and after the sleep
 * `kill_at` the caller, or with `kill_process` its process, is sent
 * SIGKILL. 0, as the kernel's wait returns once `cond` holds. */
static int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    process_t *p;

    check(wq == &execs.gone && arg == &execs, "the exec waits on its own process's queue");
    (void)name;
    while (!cond(arg)) {
        check(execs.exec_task == current_proc, "the exec is its process's exec_task as it waits");
        if (++sleeps == 16) {
            printf("endothers: fail: the exec's wait for the other threads never ends\n");
            exit(1);
        }
        for (p = proc_list; p; p = p->next) {
            if (p->tg == &execs && p != current_proc && p->state != PROC_ZOMBIE && !p->held &&
                (p->sig_pending & KSIG_BIT(SIGKILL))) {
                p->state = PROC_ZOMBIE;
                execs.live--;
            }
        }
        if (sleeps == kill_at) {
            if (kill_process) {
                execs.pending |= KSIG_BIT(SIGKILL);
            } else {
                current_proc->sig_pending |= KSIG_BIT(SIGKILL);
            }
        }
    }
    return 0;
}

#include "endothers.inc"

static struct tgroup groups[6];
static process_t init, idle, kthread, marked, ending, unused, running;

/** What the model's kernel threads run, which only marks them. */
static void kthread_main(void)
{}

/** Put `p` on the list, alone in thread group `tg`, with `space_root`,
 * `in_syscall` and `state`. */
static void add(process_t *p, int tg, uint32_t pid, uintptr_t space_root, uint32_t in_syscall,
                proc_state_t state)
{
    p->tg = &groups[tg];
    p->pid = p->tgid = pid;
    p->space_root = space_root;
    p->in_syscall = in_syscall;
    p->state = state;
    p->next = proc_list;
    proc_list = p;
}

/** A fresh list with init running, the idle record, a kernel thread and one
 * marked in a call. */
static void begin(void)
{
    static const process_t none;

    init = idle = kthread = marked = ending = unused = running = none;
    proc_list = NULL;
    switches = 0;
    add(&idle, 1, 0, 0, 0, PROC_READY);
    add(&kthread, 2, 5, 0, 0, PROC_BLOCKED);
    kthread.kthread_entry = kthread_main;
    add(&marked, 5, 6, 0, 1, PROC_BLOCKED);
    marked.kthread_entry = kthread_main;
    add(&init, 0, 1, 0x1000, 1, PROC_RUNNING);
    current_proc = &init;
    proc_idle = &idle;
}

/** The kernel threads and the idle record stay, untouched. */
static void kept(void)
{
    check(!kthread.signalled && !kthread.released && kthread.state == PROC_BLOCKED,
          "a kernel thread is left alone");
    check(!marked.signalled && !marked.released && marked.state == PROC_BLOCKED,
          "a kernel thread marked in a call is left alone");
    check(!idle.signalled && !idle.released, "the idle record is left alone");
}

static process_t caller, sibling, held, other;

/** A fresh list of `execs`: `caller` running and `sibling`, which ends on
 * SIGKILL, with `held`, which no signal ends, when `with_held` is set. */
static void exec_begin(int with_held)
{
    static const process_t none;
    static const struct tgroup empty;
    process_t *each[3];
    int i, n = with_held ? 3 : 2;

    execs = empty;
    caller = sibling = held = other = none;
    each[0] = &caller;
    each[1] = &sibling;
    each[2] = &held;
    held.held = 1;
    proc_list = NULL;
    for (i = 0; i < n; i++) {
        each[i]->tg = &execs;
        each[i]->pid = (uint32_t)(20 + i);
        each[i]->tgid = 20;
        each[i]->state = i == 0 ? PROC_RUNNING : PROC_BLOCKED;
        each[i]->next = proc_list;
        proc_list = each[i];
    }
    execs.live = (uint32_t)n;
    current_proc = &caller;
    sleeps = kill_at = kill_process = 0;
}

/** Run the exec rows. */
static void exec_rows(void)
{
    int kill;

    exec_begin(0);
    check(exec_one_thread() == 0 && sleeps == 1, "the exec waits once for the others to end");
    check(sibling.signalled == SIGKILL && sibling.state == PROC_ZOMBIE && execs.live == 1,
          "the exec sends the others SIGKILL and goes on once they have ended");
    check(!caller.signalled && execs.exec_task == NULL, "the exec clears exec_task once alone");

    for (kill = 0; kill < 2; kill++) {
        exec_begin(1);
        kill_at = 1;
        kill_process = kill;
        check(exec_one_thread() == -EAGAIN && sleeps == 1,
              kill ? "SIGKILL for the process ends the exec's wait with -EAGAIN"
                   : "SIGKILL for the caller ends the exec's wait with -EAGAIN");
        check(execs.exec_task == NULL, "the exec a SIGKILL ends clears exec_task");
        check(held.signalled == SIGKILL && held.state == PROC_BLOCKED && execs.live == 2,
              "a thread held in a wait no signal ends was sent SIGKILL and has not ended");
    }

    exec_begin(0);
    execs.exiting = 1;
    check(exec_one_thread() == -EAGAIN && sleeps == 0 && !sibling.signalled,
          "an ending process's exec gives -EAGAIN at once and sends nothing");
    check(execs.exec_task == NULL, "an ending process's exec leaves exec_task alone");

    exec_begin(0);
    execs.exec_task = &other;
    check(exec_one_thread() == -EAGAIN && sleeps == 0 && !sibling.signalled,
          "a second exec gives -EAGAIN at once and sends nothing");
    check(execs.exec_task == &other, "a second exec leaves exec_task to the first");
}

/** Run the four rows of an init's end and the exec rows: 0 when every row
 * passes, 1 otherwise. */
int main(void)
{
    begin();
    proc_end_others();
    check(switches == 0, "a kernel thread marked in a call is not waited for");
    kept();

    begin();
    add(&ending, 3, 7, 0, 1, PROC_BLOCKED);
    add(&unused, 4, 9, 0, 1, PROC_UNUSED);
    proc_end_others();
    check(ending.released == 1 && switches >= 1,
          "a process in the call that ends it is waited for until it is a zombie and reaped");
    check(!ending.signalled, "a process in the call that ends it is sent no signal");
    check(!unused.signalled && !unused.released,
          "a record another reaper has taken off the zombies is left to it");
    kept();

    begin();
    add(&running, 3, 8, 0x2000, 0, PROC_READY);
    proc_end_others();
    check(running.signalled == SIGKILL && running.released == 1,
          "a process that runs in its address space is sent SIGKILL and reaped");
    kept();

    begin();
    add(&running, 3, 8, 0x2000, 0, PROC_RUNNING);
    proc_end_others();
    check(running.signalled == SIGKILL && running.released == 1,
          "a process running on another processor takes its SIGKILL while init waits with the "
          "big lock let go, and is reaped");
    kept();

    exec_rows();

    if (!fails) {
        printf("endothers: result: pass: every other user process ended and reaped, and an "
               "exec's wait for the other threads ended\n");
    }
    return fails != 0;
}
