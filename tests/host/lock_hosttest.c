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
 * a conversion lets go first, and a description's end frees its locks.
 * Returns the failed-check count.
 */

#include "fs/lock.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("lock-hosttest: fail: line %d: %s\n", __LINE__, #cond); \
            fails++;                                                   \
        }                                                              \
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

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

static const struct lock_key file = {(const void *)0x1000, 42};
static const struct lock_key other = {(const void *)0x1000, 43};

static int32_t as(uint32_t pid, struct open_file *ofd, int cmd, int type, uint64_t start,
                  uint64_t end, struct lock_req *out)
{
    struct lock_req req;

    process.pid = pid;
    req.type = type;
    req.start = start;
    req.end = end;
    req.pid = 0;
    {
        int32_t rc = lock_record(&file, ofd, cmd, &req);

        if (out) {
            *out = req;
        }
        return rc;
    }
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
    process.pid = 2;
    lock_close(&file);
    CHECK(count_locks() == 2);
    /* Its own locks give way: a read lock over both of pid 1's pieces, which
     * are one again. */
    CHECK(as(1, NULL, LOCK_SET, F_RDLCK, 0, LOCK_EOF, NULL) == 0 && count_locks() == 1);
    CHECK(as(3, NULL, LOCK_SET, F_RDLCK, 1000000, 1000000, NULL) == 0);
    CHECK(as(3, NULL, LOCK_SET, F_WRLCK, 1000001, 1000001, NULL) == -EAGAIN);
    process.pid = 1;
    lock_close(&other);
    CHECK(count_locks() == 2);
    lock_close(&file);
    process.pid = 3;
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

    process.pid = 1;
    CHECK(lock_flock(&file, &a, LOCK_EX) == 0);
    CHECK(lock_flock(&file, &b, LOCK_EX | LOCK_NB) == -EAGAIN);
    CHECK(lock_flock(&file, &b, LOCK_SH | LOCK_NB) == -EAGAIN);
    /* flock and record locks do not meet. */
    CHECK(as(2, NULL, LOCK_SET, F_WRLCK, 0, LOCK_EOF, NULL) == 0);
    process.pid = 2;
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

int main(void)
{
    records();
    descriptions();
    return fails > 100 ? 100 : fails;
}
