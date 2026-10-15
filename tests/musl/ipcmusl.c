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
 * line, and unlinking a queue's file removes its name.
 *
 * Prints `IPCMUSL OK` and exits 0 on success, and prints
 * `IPCMUSL FAIL (detail)` and exits 1 otherwise.
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

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("IPCMUSL FAIL (%s: errno %d)\n", what, errno);
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
    snprintf(key, sizeof(key), " %10d  ", id);
    if (!file_has("/proc/self/maps", "/SYSV00000000 (deleted)") ||
        !file_has("/proc/sysvipc/shm", key)) {
        return fail("/proc/self/maps and /proc/sysvipc/shm name the segment");
    }
    if (shmctl(id, IPC_RMID, NULL) != 0 || shmctl(id, IPC_STAT, &ds) != 0 ||
        !(ds.shm_perm.mode & 01000) || ds.shm_nattch != 1 || strcmp(p, "from the parent") != 0) {
        return fail("a segment removed while attached is kept, marked SHM_DEST");
    }
    if (shmdt(p) != 0 || shmctl(id, IPC_STAT, &ds) != -1 || errno != EINVAL) {
        return fail("its last detach frees it");
    }
    id = (int)syscall(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    ro = id < 0 ? (void *)-1 : (char *)syscall(SYS_shmat, id, 0, SHM_RDONLY);
    if (ro == (void *)-1 || mprotect(ro, 4096, PROT_READ | PROT_WRITE) != -1 || errno != EACCES ||
        syscall(SYS_shmdt, ro) != 0 || shmctl(id, IPC_RMID, NULL) != 0) {
        return fail("the direct calls, and a read-only attachment mprotect cannot make writable");
    }
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
    after = heap_used();
    if (before == 0 || after > before + 4096) {
        return fail("a thousand refused segments leave the kernel's heap as it was");
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
    return 0;
}
#endif

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
    while (semctl(id, 0, GETNCNT) != 1) {
        usleep(10000);
    }
    if (semop(id, &give, 1) != 0 || child_status(child) != 0 || semctl(id, 1, GETVAL) != 1) {
        return fail("a waiter let through, and a child's SEM_UNDO undone at its exit");
    }
    child = fork();
    if (child == 0) {
        _exit(semop(id, &wait0, 1) == -1 && errno == EIDRM ? 0 : 1);
    }
    while (semctl(id, 0, GETNCNT) != 1) {
        usleep(10000);
    }
    if (semctl(id, 0, IPC_RMID) != 0 || child_status(child) != 0) {
        return fail("removing the set wakes its waiter with EIDRM");
    }
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
    if (semctl(id, 0, IPC_STAT, &ds) != 0 || ds.sem_nsems != 2 || ds.sem_ctime < now - 60 ||
        ds.sem_ctime > now + 60) {
        return fail("IPC_STAT of a set of two did not give its count and its change time");
    }
    if (semop(id, &take, 1) != -1 || errno != EAGAIN) {
        return fail("a decrement past 0 with IPC_NOWAIT is EAGAIN");
    }
#if defined __x86_64__
    if (sem_wide_count(id) != 0) {
        return 1;
    }
#endif
    take.sem_flg = 0;
    if (semtimedop(id, &take, 1, &ts) != -1 || errno != EAGAIN) {
        return fail("one with a timeout is EAGAIN after it");
    }
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
    if (msgrcv(id, &m, 4, 1, 0) != -1 || errno != E2BIG || msgrcv(id, &m, 4, 1, MSG_NOERROR) != 4 ||
        memcmp(m.text, "firs", 4) != 0) {
        return fail("a longer message is E2BIG unless MSG_NOERROR cuts it");
    }
#if defined __x86_64__
    m.type = 3;
    strcpy(m.text, "eight b");
    if (msgsnd(id, &m, 8, 0) != 0 || msgrcv(id, &m, 0x100000001UL, 3, MSG_NOERROR) != 8 ||
        strcmp(m.text, "eight b") != 0) {
        return fail("a capacity past 32 bits is not cut to its low word");
    }
#endif
    if (msgrcv(id, &m, sizeof(m.text), 0, IPC_NOWAIT) != -1 || errno != ENOMSG ||
        msgctl(id, IPC_RMID, NULL) != 0) {
        return fail("an empty queue with IPC_NOWAIT is ENOMSG");
    }
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

/** Whether process `pid` is asleep, state S in /proc/<pid>/stat. */
static int sleeping(pid_t pid)
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
    return state && state[1] == ' ' && state[2] == 'S';
}

/* The kinds of System V wait: for a semaphore, for room in a full queue, and
 * for a message in an empty one. */
#define CUT_SEM 0
#define CUT_SND 1
#define CUT_RCV 2

/* One System V wait a handler with SA_RESTART interrupts: its kind, and
 * whether it is made by syscall() with the direct call rather than by musl,
 * and with a timeout. */
struct cut_wait {
    const char *name;
    int kind;
    int direct;
    int timed;
};

static const struct cut_wait cut_waits[] = {
    {"semop", CUT_SEM, 0, 0},
    {"semtimedop", CUT_SEM, 0, 1},
    {"msgsnd", CUT_SND, 0, 0},
    {"msgrcv", CUT_RCV, 0, 0},
#if !defined __x86_64__
    {"the direct semtimedop_time64", CUT_SEM, 1, 1},
    {"the direct msgsnd", CUT_SND, 1, 0},
    {"the direct msgrcv", CUT_RCV, 1, 0},
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
 * Fork a child that waits up to 3 s to see this process asleep, sends it
 * SIGUSR1, waits up to 3 s for the handler's byte on `answer`, and only then
 * ends a wait of `kind` on `id`: it gives the semaphore, takes a message from
 * the full queue, or sends one to the empty queue. The child exits 0 when
 * all of that happened, 2 when the process was never seen asleep and 3 when
 * the handler's byte did not come; it ends the wait in every case.
 */
static pid_t signal_then_give(int kind, int id, int answer)
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        struct sembuf give = {0, 1, 0};
        struct pollfd pfd;
        int tries, code = 0;
        char c;

        for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
            usleep(10000);
        }
        if (tries == 300) {
            code = 2;
        } else {
            pfd.fd = answer;
            pfd.events = POLLIN;
            if (kill(parent, SIGUSR1) != 0 || poll(&pfd, 1, 3000) != 1 ||
                read(answer, &c, 1) != 1) {
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
        printf("IPCMUSL FAIL (%s was never seen asleep)\n", w->name);
        return 1;
    }
    if (usr1_runs != 1 || WEXITSTATUS(status) != 0) {
        printf("IPCMUSL FAIL (the handler that cut %s did not run once)\n", w->name);
        return 1;
    }
    if (n != -1 || err != EINTR) {
        printf("IPCMUSL FAIL (%s cut by a handler with SA_RESTART returned %ld, errno %d, "
               "not EINTR)\n",
               w->name, n, err);
        return 1;
    }
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

/**
 * Check /dev/mqueue while the queue /ipcfiles has one message of 4 bytes: its
 * file, its status line and its entry; and a second queue whose name goes
 * with an unlink of its file. Returns 0, or 1 once the failure is reported.
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
    if (!file_has("/dev/mqueue/ipcfiles", "QSIZE:4 ")) {
        return fail("the queue's file reads as its status line");
    }
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
    q2 = mq_open("/ipcfiles2", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
    if (q2 == (mqd_t)-1 || mq_close(q2) != 0 || unlink("/dev/mqueue/ipcfiles2") != 0 ||
        mq_open("/ipcfiles2", O_RDWR) != (mqd_t)-1 || errno != ENOENT) {
        return fail("unlinking a queue's file takes its name");
    }
    mq_close(q);
    mq_unlink("/ipcfiles");
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
    for (i = 0; i < 3; i++) {
        mq_send(q, "x", 1, 0);
    }
    if (mq_send(q, "x", 1, 0) != -1 || errno != EAGAIN) {
        return fail("a full queue with O_NONBLOCK is EAGAIN");
    }
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
    pfd.fd = q;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLIN)) {
        return fail("poll reports a message");
    }
    if (mq_close(q) != 0 || mq_unlink("/ipcmusl") != 0 ||
        mq_open("/ipcmusl", O_RDWR) != (mqd_t)-1 || errno != ENOENT) {
        return fail("the name goes with mq_unlink");
    }
    return 0;
}

int main(void)
{
    if (shm_rows() || sem_rows() || msg_rows() || cut_rows() || mq_rows() || mq_files()) {
        return 1;
    }
#if defined(__x86_64__)
    if (oversized_rows()) {
        return 1;
    }
#endif
    printf("IPCMUSL OK\n");
    return 0;
}
