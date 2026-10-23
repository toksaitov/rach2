/**
 * ipcmusl: check System V IPC and POSIX message queues.
 *
 * The System V calls are made through musl, which reaches them through
 * ipc(2) on i686 and through the direct calls on x86-64, and through the
 * direct calls GNU libc makes.
 *
 * A private segment of 8192 bytes is attached, written, and read by a child
 * that attaches it by its id; IPC_STAT counts both attachments, and
 * /proc/self/maps and /proc/sysvipc/shm list it. A segment removed while
 * attached keeps its memory and the key IPC_PRIVATE until its last detach,
 * after which its id is EINVAL. A read-only attachment cannot be made
 * writable with mprotect. The direct shmget, shmat and shmdt do the same as
 * ipc's.
 *
 * A set of two semaphores takes SETVAL and GETALL, and IPC_STAT gives its
 * count and the time it changed, each where Linux's struct has it; a
 * decrement past 0 with IPC_NOWAIT is EAGAIN, one with a timeout EAGAIN after
 * it, and one that waits is let through by the parent's increment; a child's
 * SEM_UNDO decrement is undone at its exit; removing the set wakes a waiter
 * with EIDRM.
 *
 * A queue takes messages of types 2, 1 and 1: a negative type receives the
 * lowest, type 0 the first; a longer message is E2BIG unless MSG_NOERROR
 * cuts it, an empty queue with IPC_NOWAIT ENOMSG. On x86-64 a capacity past
 * 32 bits is taken whole and a count of semaphore operations past it is
 * E2BIG, neither cut to its low word; and a thousand segments of 16 TiB,
 * too large for tmpfs, are each ENOMEM and leave the kernel's heap as it
 * was, so a failed one gives back the node of its file.
 *
 * A semop, a semtimedop, a msgsnd to a full queue and a msgrcv from an empty
 * one, through musl and on i686 also through the direct calls, each wait
 * until a child sees the process asleep and sends SIGUSR1, whose handler has
 * SA_RESTART. Each must end with EINTR, as on Linux, which never restarts
 * these calls after a handler, and must not take what the child gives once
 * the handler has answered it.
 *
 * A POSIX queue of four messages of 64 bytes gives the higher priority
 * first; a full queue with O_NONBLOCK is EAGAIN; an empty one with a
 * timeout ETIMEDOUT; mq_notify's signal comes with SI_MESGQ and its value
 * when a message reaches the queue empty; poll reports a message; the
 * name goes with mq_unlink. /dev/mqueue lists the queue as a file with its
 * mode and its messages' bytes as its size, reading it gives its status
 * line, and unlinking a queue's file removes its name. fstat of a queue's
 * descriptor gives what stat of its file gives, and its /proc/self/fd link
 * reads /NAME, then "/NAME (deleted)" with no link counted once mq_unlink
 * has taken the name, as on Linux.
 *
 * Given `machine=native` (tests/native/Readme.md), it still makes the
 * thousand refusals and leaves out the comparison of the kernel's heap, since
 * the machine's services allocate from it meanwhile.
 *
 * Prints `IPCMUSL OK` and exits 0 on success, and prints
 * `IPCMUSL FAIL (detail)` and exits 1 otherwise.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order, a setup step joined to the row after it:
 * the segment's `shm-*`, the set's `sem-*`, the queue's `msg-*`, a `cut-*`
 * for each interrupted wait, the POSIX queue's `mq-*` and its files'
 * `mqueue-*`, and on x86-64 `oversized-enomem` and `oversized-heap`. The
 * branch of the group `arch` says which ran: `x86-64`, with
 * `sem-wide-count`, `msg-wide-capacity` and the two `oversized-*`, or
 * `i686`, with the three `cut-direct-*`. A native run reports
 * `oversized-heap` not run. The first failure fails its assertion and leaves
 * the rest not run.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The assertions of etude's report in the order they are checked, more than
 * probe_report.h's path holds, and how many are planned and reported. */
static const char *rows[48];
static int rows_planned, rows_reported;

/** Add the assertion `id` to the end of the planned rows. */
static void plan_row(const char *id)
{
    rows[rows_planned++] = id;
}

/** Report that the next planned row held. */
static void held(void)
{
    report_pass(rows[rows_reported++]);
}

/** In the process that began the report, report that the next planned row
 * failed with `why`, that those after it were not run, and the report's
 * end; after the end, nothing. */
static void stop(const char *why)
{
    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    if (rows_reported < rows_planned) {
        report_fail(rows[rows_reported++], why);
    }
    while (rows_reported < rows_planned) {
        report_notrun(rows[rows_reported++], "an earlier check failed");
    }
    report_end();
}

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    char why[160];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("IPCMUSL FAIL (%s)\n", why);
    stop(why);
    return 1;
}

/** Whether the file `path` has the text `needle`. */
static int file_has(const char *path, const char *needle)
{
    static char buf[8192];
    int fd = open(path, O_RDONLY);
    ssize_t n, got = 0;

    if (fd < 0) {
        return 0;
    }
    while (got < (ssize_t)sizeof(buf) - 1 &&
           (n = read(fd, buf + got, sizeof(buf) - 1 - (size_t)got)) > 0) {
        got += n;
    }
    close(fd);
    buf[got] = '\0';
    return strstr(buf, needle) != NULL;
}

/** The status of the child `pid` when it exits, or -1. */
static int child_status(pid_t pid)
{
    int status;

    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/**
 * Check a segment shared with a child: its count of attachments, its lines in
 * /proc, its removal while attached and its id after the last detach; then a
 * read-only attachment made by the direct calls. Returns 0, or 1 once the
 * failure is reported.
 */
static int shm_rows(void)
{
    struct shmid_ds ds;
    char *p, *q, *ro;
    int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    char key[32];
    pid_t child;

    if (id < 0 || (p = shmat(id, NULL, 0)) == (void *)-1) {
        return fail("shmget and shmat");
    }
    strcpy(p, "from the parent");
    child = fork();
    if (child == 0) {
        q = shmat(id, NULL, 0);
        _exit(q != (void *)-1 && strcmp(q, "from the parent") == 0 &&
                      shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 3 && shmdt(q) == 0
                  ? 0
                  : 1);
    }
    if (child_status(child) != 0) {
        return fail("a child attaches the segment, reads it and counts three attachments");
    }
    held();
    snprintf(key, sizeof(key), " %10d  ", id);
    if (!file_has("/proc/self/maps", "/SYSV00000000 (deleted)") ||
        !file_has("/proc/sysvipc/shm", key)) {
        return fail("/proc/self/maps and /proc/sysvipc/shm name the segment");
    }
    held();
    if (shmctl(id, IPC_RMID, NULL) != 0 || shmctl(id, IPC_STAT, &ds) != 0 ||
        !(ds.shm_perm.mode & 01000) || ds.shm_nattch != 1 || strcmp(p, "from the parent") != 0) {
        return fail("a segment removed while attached is kept, marked SHM_DEST");
    }
    held();
    if (shmdt(p) != 0 || shmctl(id, IPC_STAT, &ds) != -1 || errno != EINVAL) {
        return fail("its last detach frees it");
    }
    held();
    id = (int)syscall(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    ro = id < 0 ? (void *)-1 : (char *)syscall(SYS_shmat, id, 0, SHM_RDONLY);
    if (ro == (void *)-1 || mprotect(ro, 4096, PROT_READ | PROT_WRITE) != -1 || errno != EACCES ||
        syscall(SYS_shmdt, ro) != 0 || shmctl(id, IPC_RMID, NULL) != 0) {
        return fail("the direct calls, and a read-only attachment mprotect cannot make writable");
    }
    held();
    return 0;
}

#if defined(__x86_64__)
/** The bytes of the kernel's heap in use, from /proc/meminfo; 0 if absent. */
static unsigned long heap_used(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long v = 0;

    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "HeapUsed: %lu", &v) == 1) {
            break;
        }
    }
    if (f) {
        fclose(f);
    }
    return v;
}

/**
 * Check on x86-64 that a thousand segments too large for tmpfs each fail with
 * ENOMEM and leave the kernel's heap as it was. Returns 0, or 1 once the
 * failure is reported.
 */
static int oversized_rows(void)
{
    unsigned long before = heap_used(), after;
    int i;

    for (i = 0; i < 1000; i++) {
        if (shmget(IPC_PRIVATE, (size_t)1 << 44, IPC_CREAT | 0600) != -1 || errno != ENOMEM) {
            return fail("a segment of 16 TiB is refused with ENOMEM");
        }
    }
    held();
    after = heap_used();
    if (!probe_native() && (before == 0 || after > before + 4096)) {
        return fail("a thousand refused segments leave the kernel's heap as it was");
    }
    if (!probe_native()) {
        held();
    }
    return 0;
}
#endif

/**
 * Wait for semaphore 0 of `id`, which the parent lets through, then make a
 * SEM_UNDO decrement of semaphore 1 for the exit to undo. Runs in the forked
 * child. Returns 0, or 1 when a semop failed, as the exit status.
 */
static int sem_child(int id)
{
    struct sembuf take = {0, -1, 0}, undo = {1, -1, SEM_UNDO};

    return semop(id, &take, 1) == 0 && semop(id, &undo, 1) == 0 ? 0 : 1;
}

#if defined __x86_64__
/**
 * Check on x86-64 that a count of operations past 32 bits is E2BIG and runs
 * none. Returns 0, or 1 once the failure is reported.
 */
static int sem_wide_count(int id)
{
    struct sembuf give = {0, 1, 0};

    if (syscall(SYS_semop, id, &give, 0x100000001UL) != -1 || errno != E2BIG ||
        semctl(id, 0, GETVAL) != 0) {
        return fail("a count of operations past 32 bits is not E2BIG, or ran");
    }
    held();
    return 0;
}
#endif

/**
 * Wait for the child `child` to block on semaphore 0 of `id`, as GETNCNT
 * reports, looking every 10 ms. Returns 0 once it waits there. When the
 * child ends first, reaps it, removes the set, and returns 1, so that a
 * waiter that ends without blocking fails its row.
 */
static int await_waiter(int id, pid_t child)
{
    for (;;) {
        if (semctl(id, 0, GETNCNT) == 1) {
            return 0;
        }
        if (waitpid(child, NULL, WNOHANG) != 0) {
            break;
        }
        /* interval: the semaphore's waiters are counted again every 10 ms */
        usleep(10000);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    semctl(id, 0, IPC_RMID);
    return 1;
}

/**
 * Check two waiters of the set `id`: one the parent lets through, whose
 * SEM_UNDO decrement is undone at its exit, and one the set's removal wakes.
 * Removes the set. Returns 0, or 1 once the failure is reported.
 */
static int sem_waiters(int id)
{
    struct sembuf give = {0, 1, 0}, wait0 = {0, -1, 0};
    pid_t child;

    child = fork();
    if (child == 0) {
        _exit(sem_child(id));
    }
    if (await_waiter(id, child) != 0) {
        return fail("a child's semop on a semaphore of 0 did not wait");
    }
    held();
    if (semop(id, &give, 1) != 0 || child_status(child) != 0 || semctl(id, 1, GETVAL) != 1) {
        return fail("a waiter let through, and a child's SEM_UNDO undone at its exit");
    }
    held();
    child = fork();
    if (child == 0) {
        _exit(semop(id, &wait0, 1) == -1 && errno == EIDRM ? 0 : 1);
    }
    if (await_waiter(id, child) != 0) {
        return fail("a child's semop did not wait for the set's removal");
    }
    held();
    if (semctl(id, 0, IPC_RMID) != 0 || child_status(child) != 0) {
        return fail("removing the set wakes its waiter with EIDRM");
    }
    held();
    return 0;
}

/**
 * Check a set of two semaphores: values, IPC_STAT, errors, a timeout, a
 * waiter let through, an undo, and a waiter woken by the set's removal.
 * Returns 0, or 1 once the failure is reported.
 */
static int sem_rows(void)
{
    struct sembuf take = {0, -1, IPC_NOWAIT};
    struct timespec ts = {0, 100000000};
    struct semid_ds ds;
    unsigned short all[2];
    int id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    time_t now = time(NULL);

    if (id < 0 || semctl(id, 1, SETVAL, 1) != 0 || semctl(id, 0, GETALL, all) != 0 || all[0] != 0 ||
        all[1] != 1) {
        return fail("semget, SETVAL and GETALL");
    }
    held();
    if (semctl(id, 0, IPC_STAT, &ds) != 0 || ds.sem_nsems != 2 || ds.sem_ctime < now - 60 ||
        ds.sem_ctime > now + 60) {
        return fail("IPC_STAT of a set of two did not give its count and its change time");
    }
    held();
    if (semop(id, &take, 1) != -1 || errno != EAGAIN) {
        return fail("a decrement past 0 with IPC_NOWAIT is EAGAIN");
    }
    held();
#if defined __x86_64__
    if (sem_wide_count(id) != 0) {
        return 1;
    }
#endif
    take.sem_flg = 0;
    if (semtimedop(id, &take, 1, &ts) != -1 || errno != EAGAIN) {
        return fail("one with a timeout is EAGAIN after it");
    }
    held();
    return sem_waiters(id);
}

/**
 * Check a System V queue's order by type, a cut message, and an empty queue.
 * Returns 0, or 1 once the failure is reported.
 */
static int msg_rows(void)
{
    struct {
        long type;
        char text[16];
    } m;
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);

    m.type = 2;
    strcpy(m.text, "second type");
    if (id < 0 || msgsnd(id, &m, 12, 0) != 0) {
        return fail("msgget and msgsnd");
    }
    m.type = 1;
    strcpy(m.text, "first type");
    if (msgsnd(id, &m, 11, 0) != 0 || msgsnd(id, &m, 11, 0) != 0) {
        return fail("two more messages");
    }
    if (msgrcv(id, &m, sizeof(m.text), -2, 0) != 11 || m.type != 1 ||
        msgrcv(id, &m, sizeof(m.text), 0, 0) != 12 || m.type != 2) {
        return fail("a negative type takes the lowest, type 0 the first");
    }
    held();
    if (msgrcv(id, &m, 4, 1, 0) != -1 || errno != E2BIG || msgrcv(id, &m, 4, 1, MSG_NOERROR) != 4 ||
        memcmp(m.text, "firs", 4) != 0) {
        return fail("a longer message is E2BIG unless MSG_NOERROR cuts it");
    }
    held();
#if defined __x86_64__
    m.type = 3;
    strcpy(m.text, "eight b");
    if (msgsnd(id, &m, 8, 0) != 0 || msgrcv(id, &m, 0x100000001UL, 3, MSG_NOERROR) != 8 ||
        strcmp(m.text, "eight b") != 0) {
        return fail("a capacity past 32 bits is not cut to its low word");
    }
    held();
#endif
    if (msgrcv(id, &m, sizeof(m.text), 0, IPC_NOWAIT) != -1 || errno != ENOMSG ||
        msgctl(id, IPC_RMID, NULL) != 0) {
        return fail("an empty queue with IPC_NOWAIT is ENOMSG");
    }
    held();
    return 0;
}

/* The runs of the SIGUSR1 handler that interrupts a System V wait, and the
 * pipe it answers the child on, -1 for none. */
static volatile int usr1_runs;
static int usr1_answer = -1;

/** Count a run of the SIGUSR1 handler and answer the child that sent it. */
static void on_usr1(int sig)
{
    (void)sig;
    usr1_runs++;
    if (usr1_answer >= 0) {
        (void)write(usr1_answer, "a", 1);
    }
}

/** The state letter /proc/<pid>/stat gives process `pid`, or 0 when it has
 * no stat file. */
static int proc_state(pid_t pid)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    state = strrchr(line, ')');
    return state && state[1] == ' ' ? state[2] : 0;
}

/**
 * Wait for process `pid` to be asleep, state S in /proc/<pid>/stat, looking
 * every 10 ms. Returns 1 once it is, and 0 when it ends first, as a zombie or
 * gone from /proc.
 */
static int seen_asleep(pid_t pid)
{
    int state;

    for (;;) {
        state = proc_state(pid);
        if (state == 'S') {
            return 1;
        }
        if (state == 0 || state == 'Z') {
            return 0;
        }
        /* interval: a process's state is looked at again every 10 ms */
        usleep(10000);
    }
}

/* The kinds of System V wait: for a semaphore, for room in a full queue, and
 * for a message in an empty one. */
#define CUT_SEM 0
#define CUT_SND 1
#define CUT_RCV 2

/* One System V wait a handler with SA_RESTART interrupts: its name, its
 * assertion, its kind, and whether it is made by syscall() with the direct
 * call rather than by musl, and with a timeout. */
struct cut_wait {
    const char *name;
    const char *id;
    int kind;
    int direct;
    int timed;
};

static const struct cut_wait cut_waits[] = {
    {"semop", "cut-semop", CUT_SEM, 0, 0},
    {"semtimedop", "cut-semtimedop", CUT_SEM, 0, 1},
    {"msgsnd", "cut-msgsnd", CUT_SND, 0, 0},
    {"msgrcv", "cut-msgrcv", CUT_RCV, 0, 0},
#if !defined __x86_64__
    {"the direct semtimedop_time64", "cut-direct-semtimedop", CUT_SEM, 1, 1},
    {"the direct msgsnd", "cut-direct-msgsnd", CUT_SND, 1, 0},
    {"the direct msgrcv", "cut-direct-msgrcv", CUT_RCV, 1, 0},
#endif
};

/* A message of a System V queue, as large as one may be. */
struct cut_msg {
    long type;
    char text[8192];
};

static struct cut_msg cut_msg;

/** Make the wait `w` on the set or queue `id`, and return its result. */
static long cut_call(const struct cut_wait *w, int id)
{
    struct sembuf take = {0, -1, 0};
    struct timespec ts = {30, 0};

    cut_msg.type = 1;
    switch (w->kind) {
    case CUT_SEM:
#if !defined __x86_64__
        if (w->direct) {
            long long ts64[2] = {30, 0};

            return syscall(SYS_semtimedop_time64, id, &take, 1, ts64);
        }
#endif
        return w->timed ? semtimedop(id, &take, 1, &ts) : semop(id, &take, 1);
    case CUT_SND:
        return w->direct ? syscall(SYS_msgsnd, id, &cut_msg, 1, 0) : msgsnd(id, &cut_msg, 1, 0);
    default:
        return w->direct ? syscall(SYS_msgrcv, id, &cut_msg, 1, 0, 0)
                         : msgrcv(id, &cut_msg, 1, 0, 0);
    }
}

/**
 * Fork a child that waits to see this process asleep (`seen_asleep`), sends
 * it SIGUSR1, waits for the handler's byte on `answer`, and only then ends a
 * wait of `kind` on `id`: it gives the semaphore, takes a message from the
 * full queue, or sends one to the empty queue. The child exits 0 when all of
 * that happened, 2 when the process was never seen asleep and 3 when the
 * signal or the read of the handler's byte failed; it ends the wait in every
 * case. A handler that never runs leaves the child and the wait asleep, and
 * the case's watchdog ends them.
 */
static pid_t signal_then_give(int kind, int id, int answer)
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        struct sembuf give = {0, 1, 0};
        struct pollfd pfd;
        int code = 0;
        char c;

        if (!seen_asleep(parent)) {
            code = 2;
        } else {
            pfd.fd = answer;
            pfd.events = POLLIN;
            if (kill(parent, SIGUSR1) != 0 || poll(&pfd, 1, -1) != 1 || read(answer, &c, 1) != 1) {
                code = 3;
            }
        }
        cut_msg.type = 1;
        if (kind == CUT_SEM) {
            semop(id, &give, 1);
        } else if (kind == CUT_SND) {
            msgrcv(id, &cut_msg, sizeof(cut_msg.text), 0, IPC_NOWAIT);
        } else {
            msgsnd(id, &cut_msg, 1, IPC_NOWAIT);
        }
        _exit(code);
    }
    return pid;
}

/**
 * Check that the wait `w`, interrupted by a handler with SA_RESTART, ends
 * with EINTR: on a set whose semaphore is 0, on a queue filled with two
 * messages of 8192 bytes, or on an empty queue. Returns 0, or 1 once the
 * failure is reported.
 */
static int cut_row(const struct cut_wait *w)
{
    int answer[2];
    int id, err, status;
    long n;
    pid_t pid;
    char why[160];

    id = w->kind == CUT_SEM ? semget(IPC_PRIVATE, 1, IPC_CREAT | 0600)
                            : msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    cut_msg.type = 1;
    if (id < 0 || pipe(answer) != 0 ||
        (w->kind == CUT_SND && (msgsnd(id, &cut_msg, sizeof(cut_msg.text), 0) != 0 ||
                                msgsnd(id, &cut_msg, sizeof(cut_msg.text), 0) != 0))) {
        return fail("setup for an interrupted System V wait");
    }
    usr1_runs = 0;
    usr1_answer = answer[1];
    pid = signal_then_give(w->kind, id, answer[0]);
    if (pid < 0) {
        return fail("fork for an interrupted System V wait");
    }
    n = cut_call(w, id);
    err = errno;
    usr1_answer = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("the child that signals and gives");
    }
    close(answer[0]);
    close(answer[1]);
    if (w->kind == CUT_SEM ? semctl(id, 0, IPC_RMID) != 0 : msgctl(id, IPC_RMID, NULL) != 0) {
        return fail("removing the object of an interrupted wait");
    }
    if (WEXITSTATUS(status) == 2) {
        snprintf(why, sizeof(why), "%s was never seen asleep", w->name);
        printf("IPCMUSL FAIL (%s)\n", why);
        stop(why);
        return 1;
    }
    if (usr1_runs != 1 || WEXITSTATUS(status) != 0) {
        snprintf(why, sizeof(why), "the handler that cut %s did not run once", w->name);
        printf("IPCMUSL FAIL (%s)\n", why);
        stop(why);
        return 1;
    }
    if (n != -1 || err != EINTR) {
        snprintf(why, sizeof(why),
                 "%s cut by a handler with SA_RESTART returned %ld, errno %d, not EINTR", w->name,
                 n, err);
        printf("IPCMUSL FAIL (%s)\n", why);
        stop(why);
        return 1;
    }
    held();
    return 0;
}

/**
 * Check each System V wait of `cut_waits` interrupted by a handler with
 * SA_RESTART. Returns 0, or 1 once the failure is reported.
 */
static int cut_rows(void)
{
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        return fail("a SIGUSR1 handler with SA_RESTART");
    }
    for (i = 0; i < sizeof(cut_waits) / sizeof(cut_waits[0]); i++) {
        if (cut_row(&cut_waits[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

/* The signal number, si_code and value of the last mq_notify signal. */
static volatile int mq_signo, mq_code, mq_value;

/** Whether the /proc/self/fd link of `fd` reads `want`. */
static int fd_link_is(int fd, const char *want)
{
    char path[32], link[64];
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) {
        return 0;
    }
    link[n] = '\0';
    return strcmp(link, want) == 0;
}

/**
 * Check the descriptor `q` of the queue /ipcfiles, whose file's stat is
 * `st` and which has one message of 4 bytes: fstat gives that stat and the
 * link reads /ipcfiles, and once mq_unlink has taken the name the link has
 * " (deleted)" after it and the stat no link. Returns 0, or 1 once the
 * failure is reported.
 */
static int mq_fd_file(mqd_t q, const struct stat *st)
{
    struct stat fst;

    if (fstat(q, &fst) != 0 || fst.st_dev != st->st_dev || fst.st_ino != st->st_ino ||
        fst.st_mode != st->st_mode || fst.st_nlink != 1 || fst.st_size != 4) {
        return fail("fstat of a queue's descriptor gives its file's stat");
    }
    held();
    if (!fd_link_is(q, "/ipcfiles")) {
        return fail("a queue's /proc/self/fd link reads /NAME");
    }
    held();
    if (mq_unlink("/ipcfiles") != 0 || !fd_link_is(q, "/ipcfiles (deleted)") ||
        fstat(q, &fst) != 0 || fst.st_nlink != 0 || fst.st_ino != st->st_ino) {
        return fail("an unlinked queue's link reads /NAME (deleted)");
    }
    held();
    return 0;
}

/**
 * Check /dev/mqueue while the queue /ipcfiles has one message of 4 bytes: its
 * file, its status line and its entry; a second queue whose name goes with
 * an unlink of its file; and the first queue's descriptor (`mq_fd_file`).
 * Returns 0, or 1 once the failure is reported.
 */
static int mq_files(void)
{
    struct stat st;
    struct dirent *d;
    DIR *dir;
    int listed = 0;
    mqd_t q = mq_open("/ipcfiles", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
    mqd_t q2;

    if (q == (mqd_t)-1 || mq_send(q, "four", 4, 0) != 0) {
        return fail("a queue for /dev/mqueue");
    }
    if (stat("/dev/mqueue/ipcfiles", &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0600 || st.st_size != 4) {
        return fail("/dev/mqueue gives the queue's file its mode and size");
    }
    held();
    if (!file_has("/dev/mqueue/ipcfiles", "QSIZE:4 ")) {
        return fail("the queue's file reads as its status line");
    }
    held();
    dir = opendir("/dev/mqueue");
    while (dir && (d = readdir(dir)) != NULL) {
        listed |= strcmp(d->d_name, "ipcfiles") == 0;
    }
    if (dir) {
        closedir(dir);
    }
    if (!listed) {
        return fail("/dev/mqueue lists the queue");
    }
    held();
    q2 = mq_open("/ipcfiles2", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
    if (q2 == (mqd_t)-1 || mq_close(q2) != 0 || unlink("/dev/mqueue/ipcfiles2") != 0 ||
        mq_open("/ipcfiles2", O_RDWR) != (mqd_t)-1 || errno != ENOENT) {
        return fail("unlinking a queue's file takes its name");
    }
    held();
    if (mq_fd_file(q, &st) != 0) {
        return 1;
    }
    mq_close(q);
    return 0;
}

/** Record the signal, si_code and value of an mq_notify signal. */
static void on_mq(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    mq_signo = sig;
    mq_code = si->si_code;
    mq_value = si->si_value.sival_int;
}

/**
 * Check a POSIX queue: priorities, a full queue, a timeout, a notification,
 * poll and the name's removal. Returns 0, or 1 once the failure is reported.
 */
static int mq_rows(void)
{
    struct mq_attr attr = {0, 4, 64, 0};
    struct sigevent ev;
    struct sigaction sa;
    struct timespec ts;
    struct pollfd pfd;
    char buf[64];
    unsigned prio;
    int i;
    mqd_t q = mq_open("/ipcmusl", O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);

    if (q == (mqd_t)-1 || mq_send(q, "low", 3, 1) != 0 || mq_send(q, "high", 4, 5) != 0) {
        return fail("mq_open and mq_send");
    }
    if (mq_receive(q, buf, sizeof(buf), &prio) != 4 || prio != 5 || memcmp(buf, "high", 4) != 0) {
        return fail("the higher priority first");
    }
    held();
    for (i = 0; i < 3; i++) {
        mq_send(q, "x", 1, 0);
    }
    if (mq_send(q, "x", 1, 0) != -1 || errno != EAGAIN) {
        return fail("a full queue with O_NONBLOCK is EAGAIN");
    }
    held();
    while (mq_receive(q, buf, sizeof(buf), NULL) > 0) {
    }
    attr.mq_flags = 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    if (mq_setattr(q, &attr, NULL) != 0 || mq_timedreceive(q, buf, sizeof(buf), NULL, &ts) != -1 ||
        errno != ETIMEDOUT) {
        return fail("an empty queue with a timeout is ETIMEDOUT");
    }
    held();
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_mq;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    ev.sigev_value.sival_int = 42;
    if (mq_notify(q, &ev) != 0 || mq_send(q, "note", 4, 0) != 0 || mq_signo != SIGUSR1 ||
        mq_code != SI_MESGQ || mq_value != 42) {
        return fail("mq_notify's signal with SI_MESGQ and its value");
    }
    held();
    pfd.fd = q;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLIN)) {
        return fail("poll reports a message");
    }
    held();
    if (mq_close(q) != 0 || mq_unlink("/ipcmusl") != 0 ||
        mq_open("/ipcmusl", O_RDWR) != (mqd_t)-1 || errno != ENOENT) {
        return fail("the name goes with mq_unlink");
    }
    held();
    return 0;
}

/** Plan each assertion of `ids`, `n` of them. */
static void plan_rows(const char *const *ids, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        plan_row(ids[i]);
    }
}

#define PLAN(ids) plan_rows(ids, sizeof(ids) / sizeof(ids[0]))

/**
 * Begin the report, with the branch of the architecture, and plan its
 * assertions in the order they are checked; a native run reports the
 * comparison of the kernel's heap not run.
 */
static void plan(void)
{
    static const char *const shm[] = {"shm-child", "shm-proc", "shm-removed-kept",
                                      "shm-last-detach", "shm-direct-readonly"};
    static const char *const sem[] = {"sem-values", "sem-stat", "sem-nowait"};
    static const char *const sem_msg[] = {
        "sem-timeout",       "sem-waiter-blocks", "sem-waiter-undo", "sem-removal-blocks",
        "sem-removal-eidrm", "msg-order",         "msg-e2big"};
    static const char *const mq[] = {
        "mq-priority", "mq-full-eagain", "mq-timeout",     "mq-notify",     "mq-poll",
        "mq-unlink",   "mqueue-file",    "mqueue-status",  "mqueue-listed", "mqueue-unlink-file",
        "mq-fstat",    "mq-fd-link",     "mq-deleted-link"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.ipcmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    PLAN(shm);
    PLAN(sem);
#if defined __x86_64__
    plan_row("sem-wide-count");
#endif
    PLAN(sem_msg);
#if defined __x86_64__
    plan_row("msg-wide-capacity");
#endif
    plan_row("msg-enomsg");
    for (i = 0; i < sizeof(cut_waits) / sizeof(cut_waits[0]); i++) {
        plan_row(cut_waits[i].id);
    }
    PLAN(mq);
#if defined __x86_64__
    plan_row("oversized-enomem");
    if (probe_native()) {
        report_notrun("oversized-heap", "the machine's services allocate from the heap meanwhile");
    } else {
        plan_row("oversized-heap");
    }
#endif
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    plan();
    if (shm_rows() || sem_rows() || msg_rows() || cut_rows() || mq_rows() || mq_files()) {
        return 1;
    }
#if defined(__x86_64__)
    if (oversized_rows()) {
        return 1;
    }
#endif
    report_end();
    printf("IPCMUSL OK\n");
    return 0;
}
