/**
 * Run epoll sets over model descriptors with the kernel's epoll and wait queue
 * code.
 *
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * `io/epoll.c` and `proc/wait_queue.c` are included whole. A descriptor is an
 * entry of one model table: an epoll set that `epoll_create1` made, or a model
 * object whose poll reports the events a row sets and names the object's own
 * wait queue, which the row wakes for the kinds of change it chooses, and
 * counts the polls of it. A wait takes no time: the model `poll_wait_queue`
 * looks at the ready list once. A user address is an offset in an arena.
 *
 * An edge-triggered registration is reported again for a wake of a kind its
 * events watch and for no other: data for input, room for output, and a
 * hangup for every registration; a set inside another is an edge of the
 * outer one for the kinds its own registrations watch. A one-shot
 * registration reports once and then nothing until EPOLL_CTL_MOD. A wait on
 * a set of many registrations polls only those a wake or an earlier report
 * put on its ready list. A wake of an object that two sets watch with
 * EPOLLEXCLUSIVE, each with a sleeper, wakes one sleeper and readies one
 * set, and with no sleeper readies both, as Linux's; EPOLLEXCLUSIVE with
 * EPOLL_CTL_MOD, with a set or with EPOLLPRI is EINVAL. An EPOLL_CTL_ADD that
 * would nest sets more than four deep below the outermost, or make a cycle,
 * is ELOOP, whether the chain would grow above the set added to, below the
 * set added, or both, and a set held by two paths counts by the longer. Five
 * sets nest, and a wait on the first reports an object in the fifth.
 *
 * A registration lasts while its open file is open, whatever became of the
 * number it was added by, and reports its file's events as in Linux; the
 * file's last close takes it (`epoll_release_file`). A registration of a set
 * holds the set in the nesting count while it lasts, for a fork's child that
 * closed its copy of the number too, and the child's waits report through
 * it as the parent's do. The last close
 * of a set takes the registrations of it in other sets, and the closes of
 * sets that hold or are held leave every list whole, which AddressSanitizer
 * checks. A fork is a second table whose descriptors share the first's open
 * files.
 */
#define WAIT_QUEUE_HOSTTEST
#include "proc/wait_queue.c"
#include "io/epoll.c"

extern int printf(const char *format, ...);

#define SLOTS 64

/* Where a row's event goes in the arena, and where a wait puts what it
 * reports. */
#define EVENT_AT 16u
#define OUT_AT   256u
#define OUT_MAX  4

struct percpu cpu_table[NR_CPUS];
static process_t proc, child;
static fd_table_t table, child_table;
static fd_entry_t entries[SLOTS], child_entries[SLOTS];
static uint8_t arena[1024];
static int failures;
/* The polls of model objects since a row last cleared the count. */
static int polls;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                            \
    do {                                                    \
        if (!(condition)) {                                 \
            printf("epoll-hosttest: failed: %s\n", detail); \
            failures++;                                     \
        }                                                   \
    } while (0)

/* A model object: the events its poll reports, and its wait queue. */
struct model {
    short ready;
    wait_queue_t wq;
};

/**
 * The poll of a model object: the events of `ready` that `events` asks for,
 * and POLLERR and POLLHUP whether asked for or not. The object's queue goes
 * in `*wq` when `wq` is not NULL.
 */
static short model_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct model *m = (struct model *)e->priv;

    polls++;
    if (wq) {
        *wq = &m->wq;
    }
    return (short)(m->ready & (events | POLLERR | POLLHUP));
}

static const struct file_ops model_ops = {NULL, NULL, model_poll, NULL, NULL, NULL, NULL, 0};

/** No wait in these rows sleeps, so nothing is scheduled. */
void schedule(void)
{}

/** No signal is pending: 0. */
int signal_pending(void)
{
    return 0;
}

/** The host has no interrupts to mask: 0. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The host has no interrupts to unmask. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The entry of `fd` while it is open. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    if (!t || fd < 0 || fd >= t->cap || t->chunks[0][fd].type == FD_TYPE_NONE) {
        return NULL;
    }
    return &t->chunks[0][fd];
}

/** The lowest free number above the standard three, with a new open file
 * description of its own. */
int fd_alloc(fd_table_t *t)
{
    int fd;

    for (fd = 3; fd < t->cap; fd++) {
        if (t->chunks[0][fd].type == FD_TYPE_NONE) {
            struct open_file *ofd = kzalloc(sizeof(*ofd));

            if (!ofd) {
                return -1;
            }
            ofd->refcount = 1;
            t->chunks[0][fd].ofd = ofd;
            return fd;
        }
    }
    return -1;
}

/**
 * The poll of `e`, through its file operations, or 0 for a descriptor without
 * one; `*wq` is the queue it names, or NULL.
 */
short poll_fd(fd_entry_t *e, short events, struct wait_queue **wq)
{
    *wq = NULL;
    return e->ops && e->ops->poll ? e->ops->poll(e, events, wq) : 0;
}

/** `poll_fd`, which a scan calls under its set's lock. */
short poll_fd_held(fd_entry_t *e, short events, struct wait_queue **wq)
{
    return poll_fd(e, events, wq);
}

/** Whether the poll of `e` runs without the big lock: a model object's
 * does not, so a scan of a set takes the lock for it. */
int file_poll_unlocked(const fd_entry_t *e)
{
    return e && e->ops && e->ops->poll && (e->ops->flags & FILE_OPS_POLL_UNLOCKED);
}

/* The depth of the model's big lock, which a scan takes and gives back. */
static int bkl_depth;

/** Take the big lock, one level deeper. */
void bkl_enter(void)
{
    bkl_depth++;
}

/** Give back one level of the big lock. */
void bkl_exit(void)
{
    bkl_depth--;
}

/** Whether the model's big lock is held. */
int bkl_held(void)
{
    return bkl_depth > 0;
}

/** One look at `ready`, since a wait here ends at once: 1 when it holds,
 * else 0, the deadline's. */
int poll_wait_queue(struct wait_queue *wq, int (*ready)(void *ctx), void *ctx,
                    struct poll_deadline *d, const char *wchan)
{
    (void)wq;
    (void)d;
    (void)wchan;
    return ready(ctx) ? 1 : 0;
}

/** Every wait has a deadline, which a model wait does not read: tick 0. */
void poll_ms_deadline(int timeout_ms, struct poll_deadline *d)
{
    (void)timeout_ms;
    d->set = 1;
    d->tick = 0;
    d->sec = 0;
    d->nsec = 0;
}

/** No row's wait has a signal mask: 0. */
int32_t poll_mask_set(uintptr_t umask, uint32_t size)
{
    (void)umask;
    (void)size;
    return 0;
}

/** No signal mask to restore: `ret`. */
int32_t poll_mask_done(int32_t ret)
{
    return ret;
}

/** Whether `len` bytes at `addr` lie in the arena. */
int user_range_ok(uintptr_t addr, size_t len)
{
    return addr <= sizeof(arena) && len <= sizeof(arena) - addr;
}

/** Copy `n` bytes from the arena at offset `src`; 0, or -EFAULT outside it. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uint32_t at = (uint32_t)(unsigned long)src;
    uint8_t *d = dst;
    uint32_t i;

    if (!user_range_ok(at, n)) {
        return -EFAULT;
    }
    for (i = 0; i < n; i++) {
        d[i] = arena[at + i];
    }
    return 0;
}

/** Copy `n` bytes to the arena at offset `dst`; 0, or -EFAULT outside it. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint32_t at = (uint32_t)(unsigned long)dst;
    const uint8_t *s = src;
    uint32_t i;

    if (!user_range_ok(at, n)) {
        return -EFAULT;
    }
    for (i = 0; i < n; i++) {
        arena[at + i] = s[i];
    }
    return 0;
}

/** Zero the system call frame `params`. */
static void clear_params(struct isr_params *params)
{
    uint8_t *p = (uint8_t *)params;
    unsigned long i;

    for (i = 0; i < sizeof(*params); i++) {
        p[i] = 0;
    }
}

/** A new epoll set's number. */
static int create(void)
{
    struct isr_params params;

    clear_params(&params);
    return (int)sys_epoll_create1(&params);
}

/** The entry of `fd` in the calling process's table. */
static fd_entry_t *slot(int fd)
{
    return &current_proc->fds->chunks[0][fd];
}

/** A model object's number. */
static int open_model(struct model *m, short ready)
{
    int fd = fd_alloc(current_proc->fds);

    m->ready = ready;
    wait_queue_init(&m->wq);
    if (fd >= 0) {
        slot(fd)->type = FD_TYPE_PIPE;
        slot(fd)->ops = &model_ops;
        slot(fd)->priv = m;
    }
    return fd;
}

/** A second number for the open file `fd` names, as dup gives. */
static int dup_fd(int fd)
{
    int n;

    for (n = 3; n < SLOTS; n++) {
        if (slot(n)->type == FD_TYPE_NONE) {
            *slot(n) = *slot(fd);
            slot(n)->ofd->refcount++;
            return n;
        }
    }
    return -1;
}

/** Close the number `fd`: the last close of its open file takes its
 * registrations from the sets they are in and then runs the close of what
 * it is, as fd_release_objects does. */
static void close_fd(int fd)
{
    fd_entry_t *e = slot(fd);
    struct open_file *ofd = e->ofd;

    if (--ofd->refcount == 0) {
        epoll_release_file(ofd);
        if (e->ops && e->ops->close) {
            e->ops->close(e);
        }
        kfree(ofd);
    }
    e->type = FD_TYPE_NONE;
    e->ops = NULL;
    e->priv = NULL;
    e->ofd = NULL;
}

/** A fork: the child's table is a copy of the parent's, each open file one
 * reference more, and the child runs. */
static void fork_model(void)
{
    int fd;

    for (fd = 0; fd < SLOTS; fd++) {
        child_entries[fd] = entries[fd];
        if (entries[fd].ofd) {
            entries[fd].ofd->refcount++;
        }
    }
    current_proc = &child;
}

/** The child's exit: its descriptors close, and the parent runs again. */
static void exit_model(void)
{
    int fd;

    current_proc = &child;
    for (fd = 3; fd < SLOTS; fd++) {
        if (child_entries[fd].ofd) {
            close_fd(fd);
        }
    }
    current_proc = &proc;
}

/** How many registrations the set `ep` has, the dropped ones gone. */
static uint32_t registrations(int ep)
{
    return ((struct epoll *)slot(ep)->priv)->count;
}

/** The operation `op` of epoll_ctl on `fd` in the set `ep`, for `events`. */
static int32_t ctl(int ep, int op, int fd, uint32_t events)
{
    struct isr_params params;
    uint32_t ev[3];

    ev[0] = events;
    ev[1] = (uint32_t)fd;
    ev[2] = 0;
    copy_to_user((void *)(unsigned long)EVENT_AT, ev, sizeof(ev));
    clear_params(&params);
    params.ebx = (uint32_t)ep;
    params.ecx = (uint32_t)op;
    params.edx = (uint32_t)fd;
    params.esi = EVENT_AT;
    return sys_epoll_ctl(&params);
}

/** EPOLL_CTL_ADD of `fd` to the set `ep` for `events`. */
static int32_t add(int ep, int fd, uint32_t events)
{
    return ctl(ep, EPOLL_CTL_ADD, fd, events);
}

/** A wait on the set `ep` with a timeout of 0: the events it reports. */
static int32_t wait_set(int ep)
{
    struct isr_params params;

    clear_params(&params);
    params.ebx = (uint32_t)ep;
    params.ecx = OUT_AT;
    params.edx = OUT_MAX;
    params.esi = 0;
    return sys_epoll_wait(&params);
}

/** A registration for output and one for input, each edge-triggered, of one
 * object: each is reported again for a wake of the kind it watches alone. */
static void edge_kinds(void)
{
    static struct model m;
    int out = create();
    int in = create();
    int fd = open_model(&m, POLLIN | POLLOUT);

    CHECK(add(out, fd, EPOLLOUT | EPOLLET) == 0 && add(in, fd, EPOLLIN | EPOLLET) == 0,
          "registrations for output and for input");
    CHECK(wait_set(out) == 1 && wait_set(in) == 1, "the first waits report output and input");
    CHECK(wait_set(out) == 0 && wait_set(in) == 0, "an edge is reported once");
    wq_wake(&m.wq, WQ_DATA);
    CHECK(wait_set(out) == 0, "a wake for data is no edge of output");
    CHECK(wait_set(in) == 1, "a wake for data is an edge of input");
    wq_wake(&m.wq, WQ_ROOM);
    CHECK(wait_set(out) == 1, "a wake for room is an edge of output");
    CHECK(wait_set(in) == 0, "a wake for room is no edge of input");
    wq_wake(&m.wq, 0);
    CHECK(wait_set(out) == 0 && wait_set(in) == 0, "a wake of no kind is no edge");
    wq_wake(&m.wq, WQ_HANGUP);
    CHECK(wait_set(out) == 1 && wait_set(in) == 1, "a hangup is an edge of every registration");
    wq_wake_all(&m.wq);
    CHECK(wait_set(out) == 1 && wait_set(in) == 1, "a wake of every kind is an edge of both");
    close_fd(fd);
    close_fd(in);
    close_fd(out);
}

/** An inner set watching an object for input, inside an outer set that
 * watches it edge-triggered: the outer one is told of a change the inner
 * one's registration watches, and of no other. */
static void nested_kinds(void)
{
    static struct model m;
    int outer = create();
    int inner = create();
    int fd = open_model(&m, POLLIN | POLLOUT);

    CHECK(add(inner, fd, EPOLLIN) == 0 && add(outer, inner, EPOLLIN | EPOLLET) == 0,
          "a set watching an object inside a set");
    CHECK(wait_set(outer) == 1 && wait_set(outer) == 0, "the outer set's edge is reported once");
    wq_wake(&m.wq, WQ_ROOM);
    CHECK(wait_set(outer) == 0, "room the inner set does not watch is no edge of the outer");
    wq_wake(&m.wq, WQ_DATA);
    CHECK(wait_set(outer) == 1, "data the inner set watches is an edge of the outer");
    close_fd(fd);
    close_fd(outer);
    close_fd(inner);
}

/** Six sets, each added to the one before, the outermost first: the fifth
 * add is ELOOP, and a wait on the first reports an object in the fifth. */
static void nest_outermost_first(void)
{
    static struct model m;
    int e[6];
    int fd = open_model(&m, POLLIN);
    int i;

    for (i = 0; i < 6; i++) {
        e[i] = create();
    }
    CHECK(add(e[4], fd, EPOLLIN) == 0, "an object in the fifth set");
    for (i = 0; i < 4; i++) {
        CHECK(add(e[i], e[i + 1], EPOLLIN) == 0,
              "an add that nests five sets, the outermost first, is taken");
    }
    CHECK(add(e[4], e[5], EPOLLIN) == -ELOOP,
          "a sixth set added below five, the outermost first, is ELOOP");
    CHECK(wait_set(e[0]) == 1, "an object in the fifth of five nested sets reaches the first");
    close_fd(fd);
    for (i = 0; i < 6; i++) {
        close_fd(e[i]);
    }
}

/** The same chain built from the innermost set out: the last add is ELOOP. */
static void nest_innermost_first(void)
{
    int f[6];
    int i;

    for (i = 0; i < 6; i++) {
        f[i] = create();
    }
    for (i = 4; i > 0; i--) {
        CHECK(add(f[i], f[i + 1], EPOLLIN) == 0,
              "an add that nests five sets, the innermost first, is taken");
    }
    CHECK(add(f[0], f[1], EPOLLIN) == -ELOOP,
          "a sixth set added above five, the innermost first, is ELOOP");
    for (i = 5; i >= 0; i--) {
        close_fd(f[i]);
    }
}

/** Two chains of three sets: the head of one added under the tail of the
 * other would make a chain of six, two sets above the add and two below, and
 * is ELOOP; the middle of one added there makes five and is taken. */
static void nest_both_ways(void)
{
    int g[3], h[3];
    int i;

    for (i = 0; i < 3; i++) {
        g[i] = create();
        h[i] = create();
    }
    CHECK(add(g[0], g[1], EPOLLIN) == 0 && add(g[1], g[2], EPOLLIN) == 0 &&
              add(h[0], h[1], EPOLLIN) == 0 && add(h[1], h[2], EPOLLIN) == 0,
          "two chains of three sets");
    CHECK(add(g[2], h[0], EPOLLIN) == -ELOOP, "a chain of three added under another is ELOOP");
    CHECK(add(g[2], h[1], EPOLLIN) == 0, "a chain of two added under a chain of three is taken");
    for (i = 0; i < 3; i++) {
        close_fd(g[i]);
        close_fd(h[i]);
    }
}

/** I holds A, A holds X and I holds X too, and X holds P, which holds Q: I
 * added to a sixth set would put Q five below it by the longer path. X, added
 * to I last, is the first of I's list. */
static void nest_two_paths(void)
{
    int e = create();
    int in = create();
    int a = create();
    int x = create();
    int p = create();
    int q = create();

    CHECK(add(p, q, EPOLLIN) == 0 && add(x, p, EPOLLIN) == 0 && add(a, x, EPOLLIN) == 0 &&
              add(in, a, EPOLLIN) == 0 && add(in, x, EPOLLIN) == 0,
          "a set held by two paths");
    CHECK(add(e, in, EPOLLIN) == -ELOOP,
          "a set nested five below another by the longer of two paths is ELOOP");
    close_fd(e);
    close_fd(in);
    close_fd(a);
    close_fd(x);
    close_fd(p);
    close_fd(q);
}

/** A holds B and B holds C: C added to A, or B added to A, is a cycle. */
static void nest_cycles(void)
{
    int a = create();
    int b = create();
    int c = create();

    CHECK(add(b, a, EPOLLIN) == 0 && add(c, b, EPOLLIN) == 0, "a chain of three sets");
    CHECK(add(a, c, EPOLLIN) == -ELOOP, "a cycle of three sets is ELOOP");
    CHECK(add(a, b, EPOLLIN) == -ELOOP, "a cycle of two sets is ELOOP");
    CHECK(add(a, a, EPOLLIN) == -EINVAL, "a set added to itself is EINVAL");
    close_fd(a);
    close_fd(b);
    close_fd(c);
}

/**
 * The registrations of a set last while its open file does, and go with it:
 * h0 holds h1, which holds h2, which holds h3. With h1's number closed and a
 * second number keeping h1 open, h0's registration of it holds it still, as
 * in Linux, and a set under h3 is the last the chain takes. The last closes
 * of h0 and of h2, the one holding a registration of a live set and the
 * other held by one, take their registrations with them, h1's of h2 among
 * them, and leave h3 held by nothing.
 */
static void nest_after_close(void)
{
    int h[4], k[4];
    int kept, x, i;

    for (i = 0; i < 4; i++) {
        h[i] = create();
        k[i] = create();
    }
    for (i = 0; i < 3; i++) {
        CHECK(add(h[i], h[i + 1], EPOLLIN) == 0, "a chain of four sets");
    }
    kept = dup_fd(h[1]);
    close_fd(h[1]);
    x = create();
    CHECK(add(h[3], x, EPOLLIN) == 0 && add(x, k[0], EPOLLIN) == -ELOOP,
          "a registration whose number was closed holds its set while another number keeps it");
    close_fd(x);
    close_fd(h[0]);
    close_fd(h[2]);
    CHECK(registrations(kept) == 0, "the last close of a set takes the registrations of it");
    CHECK(wait_set(kept) == 0, "a set whose registration's set was closed reports nothing");
    for (i = 0; i < 3; i++) {
        CHECK(add(k[i], k[i + 1], EPOLLIN) == 0, "a second chain of four sets");
    }
    CHECK(add(k[3], h[3], EPOLLIN) == 0, "a set whose holders have gone takes a chain above");
    x = create();
    CHECK(add(h[3], x, EPOLLIN) == -ELOOP, "the chain above a set is counted through its holders");
    close_fd(x);
    close_fd(kept);
    close_fd(h[3]);
    for (i = 0; i < 4; i++) {
        close_fd(k[i]);
    }
}

/**
 * A registration of an object lasts while the object's open file is open,
 * whatever became of its number: with the number closed and a second one
 * keeping the file open, a wait reports it still, as in Linux, and the
 * second number's close, the file's last, takes it.
 */
static void kept_open(void)
{
    static struct model m;
    int ep = create();
    int fd = open_model(&m, POLLIN);
    int second;

    CHECK(add(ep, fd, EPOLLIN) == 0 && wait_set(ep) == 1, "a ready object in a set");
    second = dup_fd(fd);
    close_fd(fd);
    CHECK(wait_set(ep) == 1 && registrations(ep) == 1,
          "a registration whose number was closed reports while another number keeps its file");
    close_fd(second);
    CHECK(registrations(ep) == 0 && wait_set(ep) == 0,
          "the last close of a registration's file takes the registration");
    close_fd(ep);
}

/**
 * A fork's child shares the parent's sets and open files: s0 holds s1, on
 * to s4, and a ready object is in s4. The child closes its copy of s1's
 * number, which the parent's keeps open, so s0's registration of s1 holds
 * s1 still: the child's add of a sixth set under s4 is ELOOP, and so is its
 * add of s0 under a sixth. The child's wait on s0 reports the object
 * through the registration, as in Linux, and so does the parent's after the
 * child has gone.
 */
static void nest_forked(void)
{
    static struct model m;
    int s[5];
    int sixth, above, fd, i;

    for (i = 0; i < 5; i++) {
        s[i] = create();
    }
    sixth = create();
    above = create();
    fd = open_model(&m, POLLIN);
    CHECK(add(s[4], fd, EPOLLIN) == 0, "a ready object in the fifth set");
    for (i = 0; i < 4; i++) {
        CHECK(add(s[i], s[i + 1], EPOLLIN) == 0, "five sets nested, each in the one before");
    }
    fork_model();
    close_fd(s[1]);
    CHECK(add(s[4], sixth, EPOLLIN) == -ELOOP,
          "a child's add under five sets, one of whose numbers it closed, is ELOOP");
    CHECK(add(above, s[0], EPOLLIN) == -ELOOP,
          "a child's add of five sets, one of whose numbers it closed, under a sixth is ELOOP");
    CHECK(wait_set(s[0]) == 1,
          "a child's wait looks into a set through a registration whose number it closed");
    exit_model();
    CHECK(wait_set(s[0]) == 1, "the parent's wait reports the object after the child has gone");
    close_fd(fd);
    close_fd(sixth);
    close_fd(above);
    for (i = 0; i < 5; i++) {
        close_fd(s[i]);
    }
}

/** A one-shot registration of a ready object: reported once, then not for
 * a wake, and again after EPOLL_CTL_MOD turns it on. */
static void one_shot(void)
{
    static struct model m;
    int ep = create();
    int fd = open_model(&m, POLLIN);

    CHECK(add(ep, fd, EPOLLIN | EPOLLONESHOT) == 0 && wait_set(ep) == 1,
          "a one-shot registration reports once");
    wq_wake(&m.wq, WQ_DATA);
    CHECK(wait_set(ep) == 0, "a one-shot registration that reported ignores a wake");
    CHECK(ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLONESHOT) == 0 && wait_set(ep) == 1,
          "EPOLL_CTL_MOD turns a one-shot registration on again");
    close_fd(fd);
    close_fd(ep);
}

/* The objects of the large set's row, and the one of them that turns ready. */
#define MANY  40
#define WOKEN 17

/**
 * A set of MANY registrations, one ready: each wait polls only the
 * registrations on the ready list, which a level-triggered report keeps
 * there and a wake puts there, and one found not ready leaves it.
 */
static void large_set(void)
{
    static struct model m[MANY];
    int fd[MANY];
    int ep = create();
    int i;

    for (i = 0; i < MANY; i++) {
        fd[i] = open_model(&m[i], i == 0 ? POLLIN : 0);
        CHECK(fd[i] >= 0 && add(ep, fd[i], EPOLLIN) == 0, "a registration in a large set");
    }
    polls = 0;
    CHECK(wait_set(ep) == 1 && polls == 1, "a wait over a large set polls its one ready entry");
    m[WOKEN].ready = POLLIN;
    wq_wake(&m[WOKEN].wq, WQ_DATA);
    polls = 0;
    CHECK(wait_set(ep) == 2 && polls == 2, "a wake puts its registration on the ready list");
    m[WOKEN].ready = 0;
    polls = 0;
    CHECK(wait_set(ep) == 1 && polls == 2, "a registration no longer ready is polled once more");
    polls = 0;
    CHECK(wait_set(ep) == 1 && polls == 1, "and leaves the ready list");
    for (i = 0; i < MANY; i++) {
        close_fd(fd[i]);
    }
    close_fd(ep);
}

/* The model sleepers of epoll_wait on two sets, and the wakes of each. */
static struct wq_waiter sleepers[2];
static int sleeper_wakes[2];

/** The wake of a model sleeper of epoll_wait: counted, and woken, so an
 * exclusive turn ends with it. */
static int sleeper_wake(struct wq_waiter *w, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    (void)kinds;
    (void)nest;
    (void)cpus;
    sleeper_wakes[w - sleepers]++;
    return 1;
}

/**
 * An object that two sets watch with EPOLLEXCLUSIVE, as Linux's: with a
 * sleeper on each set, a wake for data wakes one sleeper and readies one
 * set; with none, it readies both. EPOLLEXCLUSIVE with EPOLL_CTL_MOD, with
 * a set, or with EPOLLPRI, is EINVAL.
 */
static void exclusive(void)
{
    static struct model m;
    static process_t sleeper;
    int ep[2], i;
    int fd = open_model(&m, 0);

    for (i = 0; i < 2; i++) {
        ep[i] = create();
        CHECK(add(ep[i], fd, EPOLLIN | EPOLLEXCLUSIVE) == 0, "an exclusive registration");
        sleepers[i].proc = &sleeper;
        sleepers[i].func = sleeper_wake;
        sleepers[i].flags = WQ_EXCLUSIVE;
        wq_entry_add(&((struct epoll *)slot(ep[i])->priv)->wq, &sleepers[i]);
    }
    m.ready = POLLIN;
    wq_wake(&m.wq, WQ_DATA);
    CHECK(sleeper_wakes[0] + sleeper_wakes[1] == 1, "an exclusive wake wakes one sleeper");
    CHECK(wait_set(ep[0]) + wait_set(ep[1]) == 1, "an exclusive wake readies one set");
    for (i = 0; i < 2; i++) {
        wq_entry_remove(&sleepers[i]);
    }
    wq_wake(&m.wq, WQ_DATA);
    CHECK(wait_set(ep[0]) == 1 && wait_set(ep[1]) == 1,
          "an exclusive wake with no sleeper readies every set");
    CHECK(ctl(ep[0], EPOLL_CTL_MOD, fd, EPOLLIN) == -EINVAL,
          "EPOLL_CTL_MOD of an exclusive registration is EINVAL");
    CHECK(ctl(ep[0], EPOLL_CTL_DEL, fd, 0) == 0 && add(ep[0], fd, EPOLLIN) == 0 &&
              ctl(ep[0], EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLEXCLUSIVE) == -EINVAL,
          "EPOLL_CTL_MOD to EPOLLEXCLUSIVE is EINVAL");
    CHECK(add(ep[0], ep[1], EPOLLIN | EPOLLEXCLUSIVE) == -EINVAL,
          "EPOLLEXCLUSIVE with a set is EINVAL");
    close_fd(fd);
    fd = open_model(&m, 0);
    CHECK(add(ep[0], fd, EPOLLIN | EPOLLPRI | EPOLLEXCLUSIVE) == -EINVAL,
          "EPOLLEXCLUSIVE with EPOLLPRI is EINVAL");
    close_fd(fd);
    close_fd(ep[0]);
    close_fd(ep[1]);
}

/**
 * Set up the parent's and the child's tables and run the rows; 0 when every
 * check passed, 1 otherwise.
 */
int main(void)
{
    int i;

    for (i = 0; i < SLOTS; i++) {
        entries[i].type = FD_TYPE_NONE;
    }
    entries[0].type = entries[1].type = entries[2].type = FD_TYPE_CONSOLE;
    table.chunks[0] = entries;
    table.cap = SLOTS;
    table.refs = 1;
    proc.fds = &table;
    child_table.chunks[0] = child_entries;
    child_table.cap = SLOTS;
    child_table.refs = 1;
    child.fds = &child_table;
    current_proc = &proc;
    edge_kinds();
    nested_kinds();
    nest_outermost_first();
    nest_innermost_first();
    nest_both_ways();
    nest_two_paths();
    nest_cycles();
    nest_after_close();
    kept_open();
    nest_forked();
    one_shot();
    large_set();
    exclusive();
    printf("epoll-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
