/**
 * orphan_hosttest.c: the SIGHUP and SIGCONT of rach2/sys/proc/orphan.c,
 * which a process group gets when a process's end leaves it orphaned with a
 * member stopped, as Linux 6.12.115's kill_orphaned_pgrp sends them.
 *
 * host-deps: rach2/sys/proc/orphan.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The process list is a table of records, init's pid 1 among the parents,
 * and the terminal's group signal is a model that records each signal and
 * the group it went to. A job a shell left stopped is sent SIGHUP and then
 * SIGCONT when the shell ends and init adopts it, and nothing when it is not
 * stopped, when another member keeps a parent in another group of the
 * session, or when its parent was in another session. A process that ends
 * as the last link of its own group to the session orphans that group, its
 * own record left out; a member whose threads have all ended, or a member
 * of an earlier group that had the same id, keeps no link.
 */
#include "proc/process.h"
#include "proc/tgroup.h"
#include "lib/ksignal.h"
#include "lib/kstring.h"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

process_t *proc_list;
volatile uint32_t init_pid = 1;

static process_t procs[8];
static struct tgroup groups[8];
static int failures;
/* The signals the model sent, in order, with each one's group and serial. */
static int sent[8], sent_count;
static int sent_pgid[8];
static uint64_t sent_serial[8];

#define CHECK(c, detail)                                       \
    do {                                                       \
        if (!(c)) {                                            \
            dprintf(2, "orphan-hosttest: fail: %s\n", detail); \
            failures++;                                        \
        }                                                      \
    } while (0)

/** The record of the process `pid` in the table, or NULL. */
process_t *get_process(uint32_t pid)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->pid == pid && p->state != PROC_UNUSED) {
            return p;
        }
    }
    return NULL;
}

/** The terminal's group signal: record `signo` and its group. */
void tty_signal_group(int pgid, uint64_t serial, int signo)
{
    if (sent_count < 8) {
        sent[sent_count] = signo;
        sent_pgid[sent_count] = pgid;
        sent_serial[sent_count] = serial;
    }
    sent_count++;
}

/** Empty the table and the record of signals. */
static void reset(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        k_memset(&procs[i], 0, sizeof(procs[i]));
        k_memset(&groups[i], 0, sizeof(groups[i]));
    }
    proc_list = NULL;
    sent_count = 0;
}

/** Add a running single-threaded process `pid` of group `pgid` of the
 * lifetime `serial` and session `sid`, whose parent is `ppid`. */
static process_t *add(int i, uint32_t pid, uint32_t pgid, uint64_t serial, uint32_t sid,
                      uint32_t ppid)
{
    process_t *p = &procs[i];

    p->pid = pid;
    p->tgid = pid;
    p->pgid = pgid;
    p->pgrp_serial = serial;
    p->sid = sid;
    p->parent_pid = ppid;
    p->state = PROC_BLOCKED;
    p->tg = &groups[i];
    p->tg->leader = p;
    p->tg->live = 1;
    p->next = proc_list;
    proc_list = p;
    return p;
}

/** Stop the process `p` as a stop signal does. */
static void stop(process_t *p)
{
    p->state = PROC_STOPPED;
    p->tg->stop = SIGTSTP;
}

/** Whether the model sent SIGHUP and then SIGCONT to `pgid` of `serial`,
 * and nothing else. */
static int hup_cont(int pgid, uint64_t serial)
{
    return sent_count == 2 && sent[0] == SIGHUP && sent[1] == SIGCONT && sent_pgid[0] == pgid &&
           sent_pgid[1] == pgid && sent_serial[0] == serial && sent_serial[1] == serial;
}

/** A shell (10) ends with its job (20) stopped: init adopts the job. */
static void adopted_rows(void)
{
    process_t *shell, *job, *other;

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 20, 20, 10, 10);
    stop(job);
    job->parent_pid = 1;
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(hup_cont(20, 20), "adopted: a stopped job gets SIGHUP and then SIGCONT");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 20, 20, 10, 1);
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(sent_count == 0, "adopted: a running job gets nothing");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 20, 20, 10, 1);
    stop(job);
    add(3, 30, 30, 30, 10, 10);
    add(4, 21, 20, 20, 10, 30);
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(sent_count == 0, "adopted: a member with a parent in another group keeps the link");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 20, 20, 11, 1);
    stop(job);
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(sent_count == 0, "adopted: a parent in another session was no link");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 10, 10, 10, 1);
    stop(job);
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(sent_count == 0, "adopted: a parent in the child's own group was no link");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    job = add(2, 20, 20, 20, 10, 1);
    stop(job);
    other = add(3, 30, 30, 30, 10, 10);
    add(4, 21, 20, 7, 10, 30);
    pgrp_orphan_signal(job, shell, NULL);
    CHECK(hup_cont(20, 20), "adopted: a member of an earlier group of the id keeps no link");
    (void)other;
}

/** A process (30) ends as its group's last link to the session. */
static void ending_rows(void)
{
    process_t *shell, *ending, *zombie;

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    ending = add(2, 30, 30, 30, 10, 10);
    stop(add(3, 31, 30, 30, 10, 30));
    pgrp_orphan_signal(ending, shell, ending);
    CHECK(hup_cont(30, 30), "ending: the last link orphans its stopped group");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    ending = add(2, 30, 30, 30, 10, 10);
    stop(add(3, 31, 30, 30, 10, 10));
    pgrp_orphan_signal(ending, shell, ending);
    CHECK(sent_count == 0, "ending: another member's link keeps the group");

    reset();
    add(0, 1, 1, 1, 1, 0);
    shell = add(1, 10, 10, 10, 10, 1);
    ending = add(2, 30, 30, 30, 10, 10);
    stop(add(3, 31, 30, 30, 10, 30));
    zombie = add(4, 32, 30, 30, 10, 10);
    zombie->state = PROC_ZOMBIE;
    zombie->tg->live = 0;
    pgrp_orphan_signal(ending, shell, ending);
    CHECK(hup_cont(30, 30), "ending: a member whose threads have ended keeps no link");

    reset();
    add(0, 1, 1, 1, 1, 0);
    ending = add(2, 30, 30, 30, 10, 1);
    stop(add(3, 31, 30, 30, 10, 30));
    pgrp_orphan_signal(ending, get_process(1), ending);
    CHECK(sent_count == 0, "ending: a child of init in another session changes nothing");
}

int main(void)
{
    adopted_rows();
    ending_rows();
    printf("orphan-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
