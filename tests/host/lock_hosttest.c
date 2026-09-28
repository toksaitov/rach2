/**
 * Host test for advisory file locks (fs/lock.c).
 *
 * host-deps:
 *
 * Includes the source with the wait queue and the heap stood in by the host.
 * A record lock of one process blocks another's over any byte it covers and
 * F_GETLK reports it; an unlock of its middle splits it and frees the middle;
 * a lock of the same process replaces its own and joins the ranges it
 * touches; a lock to the end of the file covers bytes past any size; a close
 * drops the closing process's locks on that file only. An open file
 * description lock conflicts with another description's and with a
 * process's record lock. A flock lock of one description blocks another's,
 * a conversion lets go first, and a description's end frees its locks. A
 * file that moves takes its locks, and the waits for them, with it.
 * Returns the failed-check count.
 */

#include "fs/lock.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("lock-hosttest: fail: line %d: %s\n", __LINE__, #cond); \
            fails++;                                                       \
        }                                                                  \
    } while (0)

static process_t process;
process_t *current_proc = &process;

void *kzalloc(size_t n)
{
    return calloc(1, n);
}

void kfree(void *p)
{
    free(p);
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* What other processes do while a caller sleeps, one step for each sleep in
 * the order the sleeps come; a sleep with no step left is ended by a
 * signal. A step may itself wait, and its sleeps take the steps after it. */
static void (*steps[4])(void);
static int step;

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    while (!cond(arg)) {
        if (step >= 4 || !steps[step]) {
            return -EINTR;
        }
        steps[step++]();
    }
    return 0;
}

static const struct lock_key file = {(const void *)0x1000, 42};
static const struct lock_key other = {(const void *)0x1000, 43};
/* Where `file` moves to in the rows of a move. */
static const struct lock_key there = {(const void *)0x1000, 4242};

static int32_t as_on(const struct lock_key *key, uint32_t pid, struct open_file *ofd, int cmd,
                     int type, uint64_t start, uint64_t end, struct lock_req *out)
{
    struct lock_req req;

    process.pid = process.tgid = pid;
    req.type = type;
    req.start = start;
    req.end = end;
    req.pid = 0;
    {
        int32_t rc = lock_record(key, ofd, cmd, &req);

        if (out) {
            *out = req;
        }
        return rc;
    }
}

static int32_t as(uint32_t pid, struct open_file *ofd, int cmd, int type, uint64_t start,
                  uint64_t end, struct lock_req *out)
{
    return as_on(&file, pid, ofd, cmd, type, start, end, out);
}

static int count_locks(void)
{
    const struct file_lock *l;
    int n = 0;

    for (l = locks; l; l = l->next) {
        n++;
    }
    return n;
}

static void records(void)
{
    struct lock_req got;

    CHECK(as(1, NULL, LOCK_SET, F_WRLCK, 10, 29, NULL) == 0);
    CHECK(as(2, NULL, LOCK_SET, F_RDLCK, 25, 25, NULL) == -EAGAIN);
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 0, 9, NULL) == 0);
    CHECK(as(2, NULL, LOCK_GET, F_WRLCK, 12, 12, &got) == 0 && got.type == F_WRLCK &&
          got.start == 10 && got.end == 29 && got.pid == 1);
    CHECK(as(1, NULL, LOCK_GET, F_WRLCK, 12, 12, &got) == 0 && got.type == F_UNLCK);
    CHECK(as(1, NULL, LOCK_SET, F_UNLCK, 15, 19, NULL) == 0 && count_locks() == 3);
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 15, 19, NULL) == 0);
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 12, 12, NULL) == -EAGAIN);
    /* Pid 1's read lock over everything meets pid 2's write locks. */
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, LOCK_EOF, NULL) == -EAGAIN);
    process.pid = process.tgid = 2;
    lock_close(&file);
    CHECK(count_locks() == 2);
    /* Its own locks give way: a read lock over both of pid 1's pieces, which
     * are one again. */
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, LOCK_EOF, NULL) == 0 && count_locks() == 1);
    CHECK(as(3, NULL, LOCK_SET, F_RDLCK, 1000000, 1000000, NULL) == 0);
    CHECK(as(3, NULL, LOCK_SET, F_WRLCK, 1000001, 1000001, NULL) == -EAGAIN);
    process.pid = process.tgid = 1;
    lock_close(&other);
    CHECK(count_locks() == 2);
    lock_close(&file);
    process.pid = process.tgid = 3;
    lock_close(&file);
    CHECK(count_locks() == 0);

    CHECK(as(4, NULL, LOCK_SET, F_WRLCK, 100, 109, NULL) == 0);
    CHECK(as(4, NULL, LOCK_SET, F_WRLCK, 110, 119, NULL) == 0 && count_locks() == 1);
    CHECK(as(5, NULL, LOCK_GET, F_RDLCK, 119, 119, &got) == 0 && got.start == 100 &&
          got.end == 119);
    CHECK(as(4, NULL, LOCK_SET, F_UNLCK, 0, LOCK_EOF, NULL) == 0 && count_locks() == 0);
    CHECK(as(4, NULL, LOCK_SET, 7, 0, 1, NULL) == -EINVAL);
    CHECK(as(4, NULL, LOCK_GET, F_UNLCK, 0, 1, NULL) == -EINVAL);
}

static void descriptions(void)
{
    struct open_file a, b;
    struct lock_req got;

    CHECK(as(1, &a, LOCK_SET, F_WRLCK, 0, 9, NULL) == 0);
    CHECK(as(1, &b, LOCK_SET, F_WRLCK, 5, 5, NULL) == -EAGAIN);
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 5, 5, NULL) == -EAGAIN);
    CHECK(as(1, &b, LOCK_GET, F_RDLCK, 5, 5, &got) == 0 && got.pid == -1);
    lock_ofd_gone(&a);
    CHECK(as(1, &b, LOCK_SET, F_WRLCK, 5, 5, NULL) == 0);
    lock_ofd_gone(&b);
    CHECK(count_locks() == 0);

    process.pid = process.tgid = 1;
    CHECK(lock_flock(&file, &a, LOCK_EX) == 0);
    CHECK(lock_flock(&file, &b, LOCK_EX | LOCK_NB) == -EAGAIN);
    CHECK(lock_flock(&file, &b, LOCK_SH | LOCK_NB) == -EAGAIN);
    /* flock and record locks do not meet. */
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 0, LOCK_EOF, NULL) == 0);
    process.pid = process.tgid = 2;
    lock_close(&file);
    CHECK(lock_flock(&file, &a, LOCK_SH) == 0);
    CHECK(lock_flock(&file, &b, LOCK_SH | LOCK_NB) == 0);
    CHECK(lock_flock(&file, &a, LOCK_EX | LOCK_NB) == -EAGAIN);
    /* The conversion let go of a's shared lock before it failed. */
    CHECK(count_locks() == 1);
    CHECK(lock_flock(&file, &a, 3) == -EINVAL);
    lock_ofd_gone(&b);
    CHECK(count_locks() == 0 && !lock_any());
}

static int32_t cycle_result;

/** Pid 2 lets go of its read lock of byte 0. */
static void two_unlocks(void)
{
    CHECK(as(2, NULL, LOCK_SET, F_UNLCK, 0, 0, NULL) == 0);
}

/** Pid 1 waits for byte 1, which pid 3 holds. */
static void one_waits(void)
{
    cycle_result = as(1, NULL, LOCK_WAIT, F_WRLCK, 1, 1, NULL);
}

/**
 * A waiter's record follows the lock it waits for. Pids 1 and 2 read-lock
 * byte 0, and pid 3 holds byte 1 and waits to write byte 0, behind pid 2's
 * lock first. When pid 2 lets go, pid 3 waits for pid 1, so pid 1's wait
 * for byte 1 closes a cycle and fails with EDEADLK.
 */
static void wait_cycle(void)
{
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, 0, NULL) == 0);
    CHECK(as(2, NULL, LOCK_SET, F_RDLCK, 0, 0, NULL) == 0);
    CHECK(as(3, NULL, LOCK_SET, F_WRLCK, 1, 1, NULL) == 0);
    steps[0] = two_unlocks;
    steps[1] = one_waits;
    steps[2] = NULL;
    step = 0;
    CHECK(as(3, NULL, LOCK_WAIT, F_WRLCK, 0, 0, NULL) == -EINTR);
    CHECK(cycle_result == -EDEADLK);
    CHECK(waits == NULL);
    process.pid = process.tgid = 1;
    lock_close(&file);
    process.pid = process.tgid = 3;
    lock_close(&file);
    CHECK(count_locks() == 0);
}

static struct open_file shared, holder;
static int32_t second_result;

/** The second thread sharing a description asks for its lock too. */
static void second_asks(void)
{
    second_result = lock_flock(&file, &shared, LOCK_SH);
}

/** The other description lets its exclusive lock go. */
static void holder_lets_go(void)
{
    CHECK(lock_flock(&file, &holder, LOCK_UN) == 0);
}

/**
 * Two threads that share a description and wait behind another
 * description's exclusive lock end with one lock between them: the second
 * to wake finds the one the first took, so one LOCK_UN lets the file go.
 */
static void shared_wait(void)
{
    CHECK(lock_flock(&file, &holder, LOCK_EX) == 0);
    steps[0] = second_asks;
    steps[1] = holder_lets_go;
    steps[2] = NULL;
    step = 0;
    CHECK(lock_flock(&file, &shared, LOCK_SH) == 0 && second_result == 0);
    CHECK(count_locks() == 1);
    CHECK(lock_flock(&file, &shared, LOCK_UN) == 0 && count_locks() == 0);
}

/**
 * A file that moves takes its locks with it. Pid 1's record lock, an OFD
 * lock and a flock lock on `file` move to `there`, where each meets another
 * owner's lock, and none is left on `file`, which is now another file's key.
 */
static void moves(void)
{
    struct open_file a, b;

    CHECK(as(1, NULL, LOCK_SET, F_WRLCK, 0, 9, NULL) == 0);
    CHECK(as(1, &a, LOCK_SET, F_WRLCK, 20, 29, NULL) == 0);
    CHECK(lock_flock(&file, &a, LOCK_EX) == 0);
    lock_repoint(file.mnt, file.ino, there.ino);
    CHECK(as_on(&there, 2, NULL, LOCK_SET, F_WRLCK, 5, 5, NULL) == -EAGAIN);
    CHECK(as_on(&there, 2, &b, LOCK_SET, F_WRLCK, 25, 25, NULL) == -EAGAIN);
    CHECK(lock_flock(&there, &b, LOCK_EX | LOCK_NB) == -EAGAIN);
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 0, 29, NULL) == 0);
    CHECK(lock_flock(&file, &b, LOCK_EX | LOCK_NB) == 0);
    process.pid = process.tgid = 1;
    lock_close(&there);
    process.pid = process.tgid = 2;
    lock_close(&file);
    lock_ofd_gone(&a);
    lock_ofd_gone(&b);
    CHECK(count_locks() == 0);
}

/** The file pid 2 waits on moves to `there`. */
static void file_moves(void)
{
    lock_repoint(file.mnt, file.ino, there.ino);
}

/** Pid 1 lets go of byte 0 where the file is now. */
static void one_unlocks_there(void)
{
    CHECK(as_on(&there, 1, NULL, LOCK_SET, F_UNLCK, 0, 0, NULL) == 0);
}

/** The description that holds the file's flock lock lets it go there. */
static void holder_lets_go_there(void)
{
    CHECK(lock_flock(&there, &holder, LOCK_UN) == 0);
}

/**
 * A wait goes where the file goes. Pid 2 waits for byte 0 of `file`, which
 * pid 1 holds; the file moves, and pid 1 lets go where it is now. The wait
 * must meet pid 1's lock after the move, sleeping until the unlock, and take
 * its lock where the file is, leaving `file` free, and a flock wait of
 * another description must do the same.
 */
static void moving_waits(void)
{
    struct open_file c;

    CHECK(as(1, NULL, LOCK_SET, F_WRLCK, 0, 0, NULL) == 0);
    steps[0] = file_moves;
    steps[1] = one_unlocks_there;
    steps[2] = NULL;
    step = 0;
    CHECK(as(2, NULL, LOCK_WAIT, F_WRLCK, 0, 0, NULL) == 0 && step == 2);
    CHECK(as_on(&there, 3, NULL, LOCK_SET, F_WRLCK, 0, 0, NULL) == -EAGAIN);
    CHECK(as(3, NULL, LOCK_SET, F_WRLCK, 0, 0, NULL) == 0);
    process.pid = process.tgid = 2;
    lock_close(&there);
    process.pid = process.tgid = 3;
    lock_close(&file);
    CHECK(count_locks() == 0 && waiting_keys == NULL);

    CHECK(lock_flock(&file, &holder, LOCK_EX) == 0);
    steps[0] = file_moves;
    steps[1] = holder_lets_go_there;
    steps[2] = NULL;
    step = 0;
    CHECK(lock_flock(&file, &shared, LOCK_EX) == 0 && step == 2);
    CHECK(lock_flock(&there, &c, LOCK_EX | LOCK_NB) == -EAGAIN);
    CHECK(lock_flock(&file, &c, LOCK_EX | LOCK_NB) == 0);
    lock_ofd_gone(&shared);
    lock_ofd_gone(&c);
    CHECK(count_locks() == 0 && waiting_keys == NULL);
}

int main(void)
{
    records();
    descriptions();
    wait_cycle();
    shared_wait();
    moves();
    moving_waits();
    return fails > 100 ? 100 : fails;
}
