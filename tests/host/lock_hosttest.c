/**
 * lock_hosttest.c: the advisory file locks of rach2/sys/fs/lock.c, record
 * locks, open file description locks and flock locks.
 *
 * host-deps:
 *
 * The test includes the source whole, with the heap and the wait queue
 * given by the host. Each process of a row has a descriptor table of its
 * own, the owner of its record locks, except where a row makes two share
 * one. A sleep runs what the row's other processes do meanwhile, as steps
 * the row sets before it.
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
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};
/* The descriptor table of each process a row names, by its pid, the owner
 * of its record locks: a table of its own, or the one `table_of` names for
 * it while a row has it share one. */
static fd_table_t tables[10];
static fd_table_t *table_of[10];

/** The kernel heap's zeroed allocation, from the C library. */
void *kzalloc(size_t n)
{
    return calloc(1, n);
}

/** Free what `kzalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/** A wake, which does nothing: every sleep runs its step itself. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* What other processes do while a caller sleeps, one step for each sleep in
 * the order the sleeps come; a sleep with no step left is ended by a
 * signal. A step may itself wait, and its sleeps take the steps after it. */
static void (*steps[4])(void);
static int step;

/** Sleep until `cond(arg)` holds, running the next of `steps` at each
 *  sleep. Returns 0, or -EINTR when no step is left. */
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

static const struct lock_key file = {(const void *)0x1000, 42, 42};
static const struct lock_key other = {(const void *)0x1000, 43, 43};
/* Where `file` moves to in the rows of a move. */
static const struct lock_key there = {(const void *)0x1000, 4242, 4242};

/** Run as process `pid`, with the table `table_of` names for it or its own. */
static void be(uint32_t pid)
{
    process.pid = process.tgid = pid;
    process.fds = table_of[pid] ? table_of[pid] : &tables[pid];
}

/**
 * Make the record-lock request `cmd` of `type` over bytes `start` to `end`
 * of the file `key` as process `pid`, through the description `ofd` for an
 * open file description lock or NULL for a process's lock. Returns what
 * `lock_record` returns, and stores the request as it answered in `out`
 * when `out` is not NULL.
 */
static int32_t as_on(const struct lock_key *key, uint32_t pid, struct open_file *ofd, int cmd,
                     int type, uint64_t start, uint64_t end, struct lock_req *out)
{
    struct lock_req req;

    be(pid);
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

/** `as_on` for the file `file`. */
static int32_t as(uint32_t pid, struct open_file *ofd, int cmd, int type, uint64_t start,
                  uint64_t end, struct lock_req *out)
{
    return as_on(&file, pid, ofd, cmd, type, start, end, out);
}

/** The number of locks held, of every file and kind. */
static int count_locks(void)
{
    const struct file_lock *l;
    int n = 0;

    for (l = locks; l; l = l->next) {
        n++;
    }
    return n;
}

/**
 * Record locks of separate owners. A write lock of pid 1 blocks pid 2's
 * read lock over any byte of it, and F_GETLK in pid 2 reports it with its
 * range and pid, while pid 1 finds no conflict with its own lock. An unlock
 * of the middle splits the lock in two and frees the middle for pid 2. A
 * close by pid 2 drops its locks on the file only; pid 1's read lock over
 * the whole file then meets no other owner's lock, replaces pid 1's pieces
 * with one lock, and blocks a write past any size. A close of another file
 * drops nothing. Two adjacent locks of one owner join, and F_GETLK reports
 * the joined range. A lock type out of range, and F_GETLK of F_UNLCK, are
 * EINVAL.
 */
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
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, LOCK_EOF, NULL) == -EAGAIN);
    be(2);
    lock_close(&file);
    CHECK(count_locks() == 2);
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, LOCK_EOF, NULL) == 0 && count_locks() == 1);
    CHECK(as(3, NULL, LOCK_SET, F_RDLCK, 1000000, 1000000, NULL) == 0);
    CHECK(as(3, NULL, LOCK_SET, F_WRLCK, 1000001, 1000001, NULL) == -EAGAIN);
    be(1);
    lock_close(&other);
    CHECK(count_locks() == 2);
    lock_close(&file);
    be(3);
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

/**
 * Open file description locks and flock locks. A description's record lock
 * conflicts with another description's and with a process's, F_GETLK
 * reports it with pid -1, and it goes with its description
 * (`lock_ofd_gone`). A flock lock of one description blocks another's
 * exclusive and shared locks, and does not meet a record lock. Two shared
 * flock locks of two descriptions coexist, and a conversion to exclusive
 * that fails has already let go of the shared lock it had. An operation
 * that is not one of LOCK_SH, LOCK_EX and LOCK_UN is EINVAL.
 */
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

    be(1);
    CHECK(lock_flock(&file, &a, LOCK_EX) == 0);
    CHECK(lock_flock(&file, &b, LOCK_EX | LOCK_NB) == -EAGAIN);
    CHECK(lock_flock(&file, &b, LOCK_SH | LOCK_NB) == -EAGAIN);
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 0, LOCK_EOF, NULL) == 0);
    be(2);
    lock_close(&file);
    CHECK(lock_flock(&file, &a, LOCK_SH) == 0);
    CHECK(lock_flock(&file, &b, LOCK_SH | LOCK_NB) == 0);
    CHECK(lock_flock(&file, &a, LOCK_EX | LOCK_NB) == -EAGAIN);
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
    be(1);
    lock_close(&file);
    be(3);
    lock_close(&file);
    CHECK(count_locks() == 0);
}

static struct open_file shared, holder;
static int32_t second_result;

/** The second thread of the shared description requests its lock too. */
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
    be(1);
    lock_close(&there);
    be(2);
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
    be(2);
    lock_close(&there);
    be(3);
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

/* The table processes 6 and 7 share, as clone with CLONE_FILES and without
 * CLONE_THREAD makes one, and two tables of one process, as threads made
 * without CLONE_FILES have. */
static fd_table_t shared_fds, thread_fds[2];

/**
 * A descriptor table is one owner, as Linux's files_struct is. Processes 6
 * and 7 share a table: 7's read lock of byte 5 replaces that byte of 6's
 * write lock of bytes 0 to 9, where another owner's would conflict, and
 * F_GETLK in process 8 names the process that took each piece. A close by
 * 7 drops every lock of the table on the file, 6's among them. The two
 * tables of one process are two owners, whose locks conflict.
 */
static void shared_table(void)
{
    struct lock_req got;

    table_of[6] = table_of[7] = &shared_fds;
    CHECK(as(6, NULL, LOCK_SET, F_WRLCK, 0, 9, NULL) == 0);
    CHECK(as(7, NULL, LOCK_SET, F_RDLCK, 5, 5, NULL) == 0);
    CHECK(as(8, NULL, LOCK_GET, F_WRLCK, 0, 0, &got) == 0 && got.type == F_WRLCK &&
          got.start == 0 && got.end == 4 && got.pid == 6);
    CHECK(as(8, NULL, LOCK_GET, F_WRLCK, 5, 5, &got) == 0 && got.type == F_RDLCK && got.pid == 7);
    be(7);
    lock_close(&file);
    CHECK(count_locks() == 0);
    CHECK(as(8, NULL, LOCK_SET, F_WRLCK, 0, 9, NULL) == 0);
    be(8);
    lock_close(&file);
    table_of[6] = table_of[7] = NULL;

    table_of[9] = &thread_fds[0];
    CHECK(as(9, NULL, LOCK_SET, F_WRLCK, 20, 20, NULL) == 0);
    table_of[9] = &thread_fds[1];
    CHECK(as(9, NULL, LOCK_SET, F_WRLCK, 20, 20, NULL) == -EAGAIN);
    table_of[9] = &thread_fds[0];
    be(9);
    lock_close(&file);
    table_of[9] = NULL;
    CHECK(count_locks() == 0);
}

static int32_t sharer_result;

/** Process 8 waits for byte 0, which the table of processes 6 and 7 holds. */
static void eight_waits(void)
{
    sharer_result = as(8, NULL, LOCK_WAIT, F_WRLCK, 0, 0, NULL);
}

/**
 * A cycle of waits through a shared table is found. Process 6 locks byte 0
 * in the table it shares with process 7, and process 8 locks byte 1; 7
 * waits for byte 1, and 8's wait for byte 0 then closes a cycle and fails
 * with EDEADLK, since the owner of the lock 8 waits for waits for 8, though
 * in another process than the one that took the lock.
 */
static void shared_cycle(void)
{
    table_of[6] = table_of[7] = &shared_fds;
    CHECK(as(6, NULL, LOCK_SET, F_WRLCK, 0, 0, NULL) == 0);
    CHECK(as(8, NULL, LOCK_SET, F_WRLCK, 1, 1, NULL) == 0);
    steps[0] = eight_waits;
    steps[1] = NULL;
    step = 0;
    CHECK(as(7, NULL, LOCK_WAIT, F_WRLCK, 1, 1, NULL) == -EINTR);
    CHECK(sharer_result == -EDEADLK);
    CHECK(waits == NULL);
    be(8);
    lock_close(&file);
    be(6);
    lock_close(&file);
    table_of[6] = table_of[7] = NULL;
    CHECK(count_locks() == 0);
}

/** Run the rows. Returns the count of failed checks, at most 100. */
int main(void)
{
    records();
    descriptions();
    wait_cycle();
    shared_wait();
    moves();
    moving_waits();
    shared_table();
    shared_cycle();
    return fails > 100 ? 100 : fails;
}
