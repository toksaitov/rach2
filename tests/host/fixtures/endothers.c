/**
 * The end of every other user process before an init that ran under the
 * debug shell hands the machine back to it (`proc_end_others`).
 *
 * tests/host/endothers_mutate.sh extracts `proc_end_others` unchanged from a
 * copy of rach2/sys/syscall_proc.c into endothers.inc. The records here have
 * the fields it reads, and the signal, the scheduler and the reap are
 * models. A switch lets the others run: a process sent SIGKILL ends, and so
 * does one asleep in the call that ends it, as one asleep in a file's release
 * does once the release is done; each is a zombie after the switch. A walk
 * that goes on switching when nothing is left to end would wait forever, so
 * a sixteenth switch ends the run.
 *
 * The first row has a process in the call that ends it, which has given up
 * its address space (`proc_exit_cleanup`): it must be waited for until it is
 * a zombie, then reaped, and sent no signal. Beside it, a kernel thread,
 * which has no address space and is in no call, and a record another reaper
 * has taken off the zombies must be left alone and not waited for. The
 * second row has a process that runs in its address space, which must be
 * sent SIGKILL, waited for and reaped.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SIGKILL 9

typedef enum { PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE } proc_state_t;

/* The field of a thread group the extracted code reads. */
struct thread_group {
    uint32_t live;
};

/* The fields of the kernel's process record the extracted code reads, and
 * the model's own: the signal posted to it and how often it was given back. */
typedef struct process {
    struct process *next;
    struct thread_group *tg;
    proc_state_t state;
    uint32_t pid;
    uint32_t tgid;
    uintptr_t space_root;
    volatile uint32_t in_syscall;
    int signalled;
    int released;
} process_t;

process_t *proc_list;
process_t *current_proc;
process_t *proc_idle;

static int fails, switches;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("endothers-proof: fail: %s\n", what);
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
 * that ends it, end and are zombies. */
static void schedule(void)
{
    process_t *p;

    if (++switches == 16) {
        printf("endothers-proof: fail: the walk waits for a record that never ends\n");
        exit(1);
    }
    for (p = proc_list; p; p = p->next) {
        if (p != current_proc && (p->state == PROC_READY || p->state == PROC_BLOCKED) &&
            (p->signalled == SIGKILL || (p->space_root == 0 && p->in_syscall))) {
            p->state = PROC_ZOMBIE;
            p->space_root = 0;
        }
    }
}

#include "endothers.inc"

static struct thread_group groups[5];
static process_t init, idle, kthread, ending, unused, running;

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

/** A fresh list with init running, the idle record and a kernel thread. */
static void begin(void)
{
    static const process_t none;

    init = idle = kthread = ending = unused = running = none;
    proc_list = NULL;
    switches = 0;
    add(&idle, 1, 0, 0, 0, PROC_READY);
    add(&kthread, 2, 5, 0, 0, PROC_BLOCKED);
    add(&init, 0, 1, 0x1000, 1, PROC_RUNNING);
    current_proc = &init;
    proc_idle = &idle;
}

/** The kernel thread and the idle record stay, untouched. */
static void kept(void)
{
    check(!kthread.signalled && !kthread.released && kthread.state == PROC_BLOCKED,
          "a kernel thread is left alone");
    check(!idle.signalled && !idle.released, "the idle record is left alone");
}

int main(void)
{
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

    if (!fails) {
        printf("endothers-proof: result: pass: every other user process ended and reaped\n");
    }
    return fails != 0;
}
