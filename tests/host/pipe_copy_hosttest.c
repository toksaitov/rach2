/**
 * Host test of pipe transfers in which another pipe operation runs while a
 * user page is resolved.
 *
 * host-deps:
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * Pipe, ring and checked-copy code are the kernel's. User addresses refer to
 * model pages; the resolver starts a peer operation before returning a
 * missing or copy-on-write page. A peer that would take the pipe's lock while
 * the caller holds it waits, and runs when the caller gives the lock back, as
 * a thread that waits for Linux's `pipe->mutex`; a close takes no lock and
 * runs at once. Waits, signals and wakeups are recorded substitutes; a wakeup
 * keeps the kinds of change it names, so a read must wake the writers for
 * room alone, a write the readers for data alone, whether the ring had data
 * before it or not, and a close the other end for a hangup.
 */
#include "ipc/pipe.c"
#include "ipc/pipe_ring.c"
#include "mm/usercopy.c"

/** The compare-and-exchange `cmpxchg_user_u32` makes, which no row reaches,
 * on the host's one thread. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

extern int printf(const char *format, ...);

#define USER_BASE  0x04000000u
#define PEER_PAGE  4
/* The user pages: a peer's transfer of a whole ring starts at PEER_PAGE. */
#define USER_PAGES (PEER_PAGE + PIPE_BUF_SIZE / PAGE_SIZE + 1)

static pipe_t ring;
static uint8_t ring_bytes[PIPE_BUF_SIZE];
/* The page buffers' slots, which a row that counts them gives the ring. */
static struct pipe_page ring_pages[2 * (PIPE_BUF_SIZE / PAGE_SIZE) + 2];
static fd_entry_t reader, writer;
static struct open_file read_ofd, write_ofd;
static uint8_t memory[USER_PAGES * PAGE_SIZE];
static int pages[USER_PAGES];
static int failures, waits, read_wakes, write_wakes, signals, resolves;
static uint32_t read_kinds, write_kinds;
static void (*resolve_action)(void);
static void (*wait_action)(void);
static uint32_t peer_count;
static int32_t peer_result;
static int peer_closes;
/* The model's thread: 1 for a row's own call, 2 for a peer. */
static int thread_now = 1;
/* A peer that waits for the pipe's lock, and the waits for it so far. */
static void (*lock_waiter)(void);
static int lock_waits;
static process_t writing_thread;
struct percpu cpu_table[NR_CPUS] = {{.current = &writing_thread}};

#define CHECK(condition, detail)                                \
    do {                                                        \
        if (!(condition)) {                                     \
            printf("pipe-copy-hosttest: failed: %s\n", detail); \
            failures++;                                         \
        }                                                       \
    } while (0)

/** The model user address `offset` bytes into the user pages, as a pointer
 * the host must not follow. */
static char *user_at(uint32_t offset)
{
    return (char *)(unsigned long)(USER_BASE + offset);
}

/** The host address of `ptr`: its byte in `memory` for a model user
 * address, `ptr` itself for any other. */
static void *host_address(const void *ptr)
{
    unsigned long addr = (unsigned long)ptr;

    if (addr >= USER_BASE && addr < USER_BASE + sizeof(memory)) {
        return memory + (addr - USER_BASE);
    }
    return (void *)ptr;
}

/** Copy `n` bytes, translating model user addresses with `host_address`. */
void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = host_address(dst);
    const uint8_t *s = host_address(src);
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/** Fill `n` bytes of `dst` with `c`, so that the test links none of the
 * kernel's string functions. */
void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

/** Run `action` as a peer: at once when the pipe's lock is free, and
 * otherwise once its holder gives it back (`sleep_lock_give`). */
static void as_peer(void (*action)(void))
{
    int was = thread_now;

    if (ring.lock.depth > 0 && ring.lock.holder != 2) {
        CHECK(lock_waiter == NULL, "one peer waits for the lock");
        lock_waiter = action;
        lock_waits++;
        return;
    }
    thread_now = 2;
    action();
    thread_now = was;
}

/** Make `l` a free lock, for pipe creation, which no row reaches. */
void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    (void)record;
    (void)flags;
    l->holder = -1;
    l->depth = 0;
}

/** Take `l` for the model's thread; a row's call or a peer finds it free,
 * since a peer that would wait runs only once it is (`as_peer`). */
void sleep_lock_take(struct sleep_lock *l)
{
    CHECK(l->depth == 0 || l->holder == thread_now, "a pipe lock is taken only when free");
    l->holder = thread_now;
    l->depth++;
}

/** Give back one take of `l`; the last runs the peer that waits for it. */
void sleep_lock_give(struct sleep_lock *l)
{
    void (*waiter)(void) = lock_waiter;

    CHECK(l->depth > 0 && l->holder == thread_now, "a pipe lock is given by its holder");
    if (--l->depth > 0) {
        return;
    }
    l->holder = -1;
    if (waiter && l == &ring.lock) {
        lock_waiter = NULL;
        as_peer(waiter);
    }
}

/**
 * Resolve the page of `addr` for a read or, with `write`, a write: 0, a
 * fault, for an address outside the model or a page marked -1, and 1
 * otherwise. A page whose state is below what the access needs (0 missing, 1
 * copy-on-write, 2 writable) first starts `resolve_action` once, as a peer
 * would run while the kernel allocates the frame (`as_peer`). A copy must
 * stay in one page.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    uint32_t page;

    CHECK(len > 0 && len <= PAGE_SIZE - (addr & (PAGE_SIZE - 1)), "one user page per copy");
    if (addr < USER_BASE || addr - USER_BASE >= sizeof(memory)) {
        return 0;
    }
    page = (addr - USER_BASE) / PAGE_SIZE;
    if (pages[page] < 0) {
        return 0;
    }
    if (pages[page] < (write ? 2 : 1)) {
        void (*action)(void) = resolve_action;

        resolves++;
        resolve_action = NULL;
        if (action) {
            as_peer(action);
        }
        pages[page] = write ? 2 : 1;
    }
    return 1;
}

/** Whether the page of `addr` is present for a read, or with `write` for
 * a write, with no resolve: 1 or 0, 0 also outside the model. A check must
 * stay in one page. */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    uint32_t page;

    CHECK(len > 0 && len <= PAGE_SIZE - (addr & (PAGE_SIZE - 1)), "one user page per check");
    if (addr < USER_BASE || addr - USER_BASE >= sizeof(memory)) {
        return 0;
    }
    page = (addr - USER_BASE) / PAGE_SIZE;
    return pages[page] >= (write ? 2 : 1);
}

/** Whether a pin may take the page of `addr`: always, since the model has
 * no region that Linux's get_user_pages refuses. */
int paging_pin_allows(struct mm *mm, uintptr_t addr, int write)
{
    (void)mm;
    (void)addr;
    (void)write;
    return 1;
}

/** A wait on a pipe's queue: run `wait_action` once, as a peer would run
 * while the caller sleeps, and return 0; -EINTR when no action is set. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*action)(void) = wait_action;

    CHECK(wq == &ring.read_wq || wq == &ring.write_wq, "wait uses pipe queue");
    CHECK(!cond(arg), "wait starts without readiness");
    waits++;
    wait_action = NULL;
    if (!action) {
        return -EINTR;
    }
    as_peer(action);
    CHECK(cond(arg), "peer makes wait ready");
    return 0;
}

/** A named wait, which the model takes as `wq_wait_event`. */
int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event(wq, cond, arg);
}

/** Count the wakeups of each end's queue and the kinds of change they name,
 * waking nothing. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    if (wq == &ring.read_wq) {
        read_wakes++;
        read_kinds |= kinds;
    } else if (wq == &ring.write_wq) {
        write_wakes++;
        write_kinds |= kinds;
    } else {
        CHECK(0, "wake uses pipe queue");
    }
}

/** A notification of every kind of change. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

/** Empty the queue `wq`, for pipe creation, which no row reaches. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Count the SIGPIPE a writer whose last reader closed posts to the writing
 * thread. */
void signal_post_thread(process_t *proc, int signo, const struct k_siginfo *info)
{
    CHECK(proc == current_proc && signo == SIGPIPE && info->code == SI_USER,
          "writer posts sigpipe");
    signals++;
}

/** A failure: the test's pipe has its ring already. */
void *kmalloc(size_t n)
{
    (void)n;
    CHECK(0, "unexpected ring allocation");
    return NULL;
}

/** A failure: no row creates a pipe. */
void *kzalloc(size_t n)
{
    (void)n;
    CHECK(0, "unexpected allocation");
    return NULL;
}

/** A failure: no row frees a pipe end. */
void kfree(void *ptr)
{
    (void)ptr;
    CHECK(0, "unexpected pipe release");
}

/** A failure: no row allocates a descriptor. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    CHECK(0, "unexpected descriptor allocation");
    return -1;
}

/** A failure: no row frees a descriptor. */
int fd_free(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    CHECK(0, "unexpected descriptor release");
    return -1;
}

/** A failure: no row opens a FIFO, whose attach alone looks an entry up
 * again. */
fd_entry_t *fd_entry_now(fd_table_t *table, fd_entry_t *e)
{
    (void)table;
    CHECK(0, "unexpected descriptor lookup");
    return e;
}

/** Reset the pipe to two nonblocking ends of an empty ring whose head and
 * tail are `head`, and every model page to writable, with 0x33 in the
 * first pages and 0x22 from `PEER_PAGE`. */
static void setup(uint32_t head)
{
    unsigned i;

    k_memset(&ring, 0, sizeof(ring));
    k_memset(&reader, 0, sizeof(reader));
    k_memset(&writer, 0, sizeof(writer));
    k_memset(&read_ofd, 0, sizeof(read_ofd));
    k_memset(&write_ofd, 0, sizeof(write_ofd));
    k_memset(memory, 0x33, sizeof(memory));
    ring.buf = ring_bytes;
    ring.size = PIPE_BUF_SIZE;
    ring.head = ring.tail = head;
    ring.read_open = ring.write_open = 1;
    reader.priv = writer.priv = &ring;
    reader.flags = FD_FLAG_READ;
    writer.flags = FD_FLAG_WRITE;
    reader.ofd = &read_ofd;
    writer.ofd = &write_ofd;
    read_ofd.status_flags = write_ofd.status_flags = OFD_NONBLOCK;
    for (i = 0; i < USER_PAGES; i++) {
        pages[i] = 2;
    }
    k_memset(memory + PEER_PAGE * PAGE_SIZE, 0x22, sizeof(memory) - PEER_PAGE * PAGE_SIZE);
    waits = read_wakes = write_wakes = signals = resolves = 0;
    read_kinds = write_kinds = 0;
    resolve_action = wait_action = NULL;
    peer_count = 0;
    peer_result = 0;
    peer_closes = 0;
    thread_now = 1;
    lock_waiter = NULL;
    lock_waits = 0;
}

/** Read `peer_count` bytes through `pipe_read` into PEER_PAGE, as a peer
 * during a resolve or a wait, its result in `peer_result`, and with
 * `peer_closes` close the writer after. */
static void peer_read(void)
{
    peer_result = pipe_read(&reader, user_at(PEER_PAGE * PAGE_SIZE), peer_count);
    if (peer_closes) {
        pipe_close(&writer);
    }
}

/** Write `peer_count` bytes from PEER_PAGE through `pipe_write`, as a peer
 * during a resolve or a wait, its result in `peer_result`. */
static void peer_write(void)
{
    peer_result = pipe_write(&writer, user_at(PEER_PAGE * PAGE_SIZE), peer_count);
}

/** Close the read end, as a peer while a writer resolves its source page. */
static void close_reader(void)
{
    pipe_close(&reader);
}

/**
 * A reader whose page is resolved while a peer would read 2 of the 8 bytes
 * holds the pipe across the resolve, as Linux's `pipe_read` holds
 * `pipe->mutex`: it copies the first 6 bytes, adjacent in the stream, and
 * the peer, which waited for the lock, reads the last 2 after it. `head` is
 * the ring's head: 0, at the end of the buffer, or at the wrap of the
 * counter; `present` is the page's state, 0 missing or 1 copy-on-write.
 */
static void read_competition(uint32_t head, int present)
{
    static const uint8_t bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t *peer = memory + PEER_PAGE * PAGE_SIZE;

    setup(head);
    pipe_ring_put(&ring, bytes, sizeof(bytes));
    pages[0] = present;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(0), 6) == 6, "a reader keeps the pipe across its resolve");
    CHECK(memory[0] == 1 && memory[2] == 3 && memory[5] == 6,
          "a reader copies the stream's first bytes");
    CHECK(lock_waits == 1 && peer_result == 2 && peer[0] == 7 && peer[1] == 8,
          "a peer's read waits for the lock and takes the bytes after");
    CHECK(pipe_available(&ring) == 0, "reader does not consume twice");
    CHECK(resolves == 1, "reader resolves lazy or cow page once");
}

/**
 * A writer whose page is resolved while a peer would write holds the pipe
 * across the resolve, as Linux's `pipe_write` holds `pipe->mutex`: its 3
 * bytes follow the 2 unread ones, and the peer's, written once the lock is
 * free, follow its own. With `fill` the peer's write fills the ring to the
 * byte. No byte already in the ring is lost.
 */
static void write_competition(int fill)
{
    uint8_t bytes[PIPE_BUF_SIZE];
    uint32_t i;
    int32_t result;
    int intact = 1;

    setup(PIPE_BUF_SIZE - 2);
    bytes[0] = bytes[1] = 0x11;
    pipe_ring_put(&ring, bytes, 2);
    pages[0] = 0;
    peer_count = fill ? PIPE_BUF_SIZE - 5 : 4;
    resolve_action = peer_write;
    result = pipe_write(&writer, user_at(0), 3);
    CHECK(result == 3, "a writer keeps the pipe across its resolve");
    CHECK(lock_waits == 1 && peer_result == (int32_t)peer_count,
          "a peer's write waits for the lock and takes the room after");
    CHECK(pipe_available(&ring) == 5 + peer_count, "writer occupancy stays bounded");
    pipe_ring_get(&ring, bytes, PIPE_BUF_SIZE);
    CHECK(bytes[0] == 0x11 && bytes[1] == 0x11, "writer preserves unread bytes");
    CHECK(bytes[2] == 0x33 && bytes[4] == 0x33, "a writer's bytes follow the unread ones");
    for (i = 0; i < peer_count; i++) {
        if (bytes[5 + i] != 0x22) {
            intact = 0;
        }
    }
    CHECK(intact, "a peer's bytes follow the writer's");
    CHECK(resolves == 1, "writer resolves lazy page once");
}

/**
 * A read whose second destination is resolved while a peer would read or
 * write keeps the pipe, as Linux's `pipe_read` holds `pipe->mutex` across
 * the fault (fs/pipe.c:264-399 in 6.12). With four page buffers of 2 bytes,
 * a read of 6 bytes into a vector of 2 and 4 copies the first three buffers,
 * adjacent in the stream, and the peer's read, once the lock is free, takes
 * the fourth; under the old read it took the second, and the read returned
 * the first, third and fourth. With one buffer of 4 bytes, a read into a
 * vector of 4 bytes whose page is resolved and 4 that cannot be written
 * returns the 4, and the peer's append of 4 merged into that buffer waits;
 * under the old read the buffer grew to 8 during the resolve, the copy of 8
 * faulted, and the read was -EFAULT with every byte kept.
 */
static void fault_holds_pipe(void)
{
    struct kernel_iovec iov[2];
    const uint8_t *peer = memory + PEER_PAGE * PAGE_SIZE;
    uint32_t i;

    setup(0);
    ring.buf_pages = ring_pages;
    ring.buf_cap = pipe_buf_slots(ring.size);
    for (i = 0; i < 8; i++) {
        memory[i] = (uint8_t)(i + 1);
    }
    for (i = 0; i < 4; i++) {
        CHECK(pipe_splice_write(&writer, user_at(2 * i), 2, 1, 2 * i) == 2,
              "a splice of 2 bytes makes a buffer");
    }
    CHECK(ring.bufs == 4, "four buffers of 2 bytes");
    iov[0].iov_base = USER_BASE + PAGE_SIZE;
    iov[0].iov_len = 2;
    iov[1].iov_base = USER_BASE + 2 * PAGE_SIZE;
    iov[1].iov_len = 4;
    pages[2] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_readv(&reader, iov, 2) == 6, "a read keeps the pipe across a later page's resolve");
    CHECK(memory[PAGE_SIZE] == 1 && memory[PAGE_SIZE + 1] == 2 && memory[2 * PAGE_SIZE] == 3 &&
              memory[2 * PAGE_SIZE + 2] == 5 && memory[2 * PAGE_SIZE + 3] == 6,
          "a read returns the stream's first 6 bytes, adjacent");
    CHECK(lock_waits == 1 && peer_result == 2 && peer[0] == 7 && peer[1] == 8,
          "a peer's read waits and takes the fourth buffer");
    CHECK(pipe_available(&ring) == 0 && ring.bufs == 0, "every buffer read once");

    setup(0);
    ring.buf_pages = ring_pages;
    ring.buf_cap = pipe_buf_slots(ring.size);
    CHECK(pipe_write(&writer, user_at(PEER_PAGE * PAGE_SIZE), 4) == 4 && ring.bufs == 1,
          "a write of 4 makes a buffer");
    iov[0].iov_base = USER_BASE + PAGE_SIZE;
    iov[0].iov_len = 4;
    iov[1].iov_base = USER_BASE + 2 * PAGE_SIZE;
    iov[1].iov_len = 4;
    pages[1] = 0;
    pages[2] = -1;
    peer_count = 4;
    resolve_action = peer_write;
    CHECK(pipe_readv(&reader, iov, 2) == 4, "a read of a buffer no append grows returns it whole");
    CHECK(memory[PAGE_SIZE] == 0x22 && memory[PAGE_SIZE + 3] == 0x22, "a read copies its buffer");
    CHECK(lock_waits == 1 && peer_result == 4, "a peer's append waits for the lock");
    CHECK(pipe_available(&ring) == 4 && ring.bufs == 1, "the peer's bytes stay queued after");
}

/**
 * A transfer rechecks the ring after a resolve or a wait: a peer's read
 * that waited for a reader's resolve finds the ring drained, and a later
 * read gives 0 once the writer closed; a write after the reader closed
 * during its resolve, which a close does without the lock, gives -EPIPE and
 * SIGPIPE; a blocking read or write waits, and a read wakes the writers for
 * room alone and a write the readers for data alone.
 */
static void state_changes(void)
{
    uint8_t bytes[PIPE_BUF_SIZE];

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(0), 2) == 2, "a reader drains the ring across its resolve");
    CHECK(peer_result == -EAGAIN && pipe_available(&ring) == 0,
          "a peer's read after it finds the ring drained");

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    peer_closes = 1;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(0), 2) == 2, "a reader takes its bytes before the close");
    CHECK(pipe_read(&reader, user_at(0), 2) == 0, "drained reader observes eof");
    CHECK(read_kinds & WQ_HANGUP, "a close wakes the readers for a hangup");

    setup(0);
    pages[0] = 0;
    resolve_action = close_reader;
    CHECK(pipe_write(&writer, user_at(0), 2) == -EPIPE, "writer rechecks reader closure");
    CHECK(signals == 1 && pipe_available(&ring) == 0, "closed reader gets no bytes");

    setup(0);
    read_ofd.status_flags = 0;
    CHECK(pipe_read(&reader, user_at(0), 1) == -EINTR, "empty read interrupted");
    peer_count = 2;
    wait_action = peer_write;
    CHECK(pipe_read(&reader, user_at(0), 8) == 2, "read rechecks after wait");
    CHECK(waits == 2 && write_wakes > 0, "reader waits and wakes writer");
    CHECK(write_kinds == WQ_ROOM, "a read wakes the writers for room alone");

    setup(0);
    write_ofd.status_flags = 0;
    pipe_ring_put(&ring, bytes, PIPE_BUF_SIZE);
    CHECK(pipe_write(&writer, user_at(0), 1) == -EINTR, "full write interrupted");
    peer_count = PIPE_BUF_SIZE;
    wait_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "write rechecks after wait");
    CHECK(waits == 2 && read_wakes == 1, "refill wakes reader");
    CHECK(read_kinds == WQ_DATA, "a write wakes the readers for data alone");
    CHECK(pipe_ring_get(&ring, bytes, sizeof(bytes)) == 3, "refill count");
}

/**
 * A blocking reader or writer that holds the pipe across its resolve
 * neither waits nor finds the ring changed: the reader takes its bytes, and
 * the writer its room, before the peer that waited for the lock writes or
 * reads; each page a write copies wakes the readers.
 */
static void readiness_changes(void)
{
    setup(0);
    read_ofd.status_flags = 0;
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_write;
    CHECK(pipe_read(&reader, user_at(0), 8) == 2, "a blocking reader keeps its bytes");
    CHECK(waits == 0 && peer_result == 2 && pipe_available(&ring) == 2,
          "a blocking reader waits for nothing, and the peer's bytes come after it");

    setup(0);
    write_ofd.status_flags = 0;
    pages[0] = 0;
    peer_count = PIPE_BUF_SIZE - 3;
    resolve_action = peer_write;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "a blocking writer keeps its room");
    CHECK(waits == 0 && peer_result == PIPE_BUF_SIZE - 3 && pipe_available(&ring) == PIPE_BUF_SIZE,
          "a blocking writer waits for nothing, and the peer after it fills the rest");
    CHECK(read_wakes == PIPE_BUF_SIZE / PAGE_SIZE + 1, "each page a write copies wakes reader");

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "a writer copies before the peer reads");
    CHECK(read_wakes == 1, "a write into a ring that has data wakes the readers once");
    CHECK(peer_result == 2 && pipe_available(&ring) == 3,
          "the peer after it reads the older bytes");
}

/** A write into a ring that already has data wakes the readers for data, so
 * an edge-triggered registration of the read end is told of each write. */
static void write_with_data(void)
{
    setup(0);
    pipe_ring_put(&ring, memory, 2);
    CHECK(pipe_write(&writer, user_at(0), 3) == 3 && pipe_available(&ring) == 5,
          "a write into a ring that has data");
    CHECK(read_wakes == 1 && read_kinds == WQ_DATA,
          "a write into a ring that has data wakes the readers for data");
}

/**
 * A read over two pages whose second faults is -EFAULT and keeps every byte
 * of the buffer it copied, as Linux's pipe_read advances a buffer only once
 * its copy is whole; one whose later page is resolved while a peer would
 * read returns the whole buffer, the peer waiting for the lock. A read of a
 * vector whose first buffer faults, through `pipe_readv`, is -EAGAIN from an
 * empty pipe and 0 from an ended one, which Linux's pipe_read answers
 * before any copy. A write of fewer bytes
 * than a page is one group, which a write commits whole, as Linux's
 * pipe_write commits a page: when its second page faults it is -EFAULT
 * with no byte added, and it leaves the group's buffer empty in the ring,
 * as Linux's pipe_write leaves the buffer it moved the head past: the
 * readers are woken, poll reports POLLIN, a later small write joins the
 * buffer, and a read that reaches it alone lets it go and is -EAGAIN. When
 * the reader closes while that page is resolved the write is -EPIPE with
 * SIGPIPE and no byte added.
 */
static void later_page(void)
{
    struct kernel_iovec iov[2];

    setup(0);
    pipe_ring_put(&ring, memory, 6);
    pages[1] = -1;
    CHECK(pipe_read(&reader, user_at(PAGE_SIZE - 2), 6) == -EFAULT,
          "a read fault in a buffer returns efault");
    CHECK(pipe_available(&ring) == 6 && write_wakes == 0,
          "a read fault keeps its buffer's bytes and wakes no writer");

    setup(0);
    pages[0] = -1;
    iov[0].iov_base = USER_BASE;
    iov[0].iov_len = 4;
    iov[1].iov_base = USER_BASE + PAGE_SIZE;
    iov[1].iov_len = 4;
    CHECK(pipe_readv(&reader, iov, 2) == -EAGAIN,
          "an empty pipe is eagain before a vector's bad first buffer faults");
    ring.write_open = 0;
    CHECK(pipe_readv(&reader, iov, 2) == 0,
          "an ended pipe is 0 before a vector's bad first buffer faults");

    setup(0);
    ring.buf_pages = ring_pages;
    ring.buf_cap = pipe_buf_slots(ring.size);
    pages[1] = -1;
    CHECK(pipe_write(&writer, user_at(PAGE_SIZE - 2), 6) == -EFAULT,
          "a write fault in the first group returns efault");
    CHECK(pipe_available(&ring) == 0 && ring.bufs == 1 && read_wakes == 1 && read_kinds == WQ_DATA,
          "a write fault publishes no byte of its group and leaves its buffer empty");
    CHECK(pipe_poll(&reader, PIPE_POLLIN, NULL) == PIPE_POLLIN,
          "a pipe with an empty buffer polls in");
    CHECK(pipe_write(&writer, user_at(0), 3) == 3 && ring.bufs == 1 && pipe_available(&ring) == 3,
          "a small write joins the empty buffer");
    CHECK(pipe_read(&reader, user_at(PEER_PAGE * PAGE_SIZE), 8) == 3 && ring.bufs == 0,
          "a read takes the joined buffer whole");
    CHECK(pipe_write(&writer, user_at(PAGE_SIZE - 2), 6) == -EFAULT && ring.bufs == 1,
          "a second write fault leaves an empty buffer");
    write_wakes = 0;
    CHECK(pipe_read(&reader, user_at(PEER_PAGE * PAGE_SIZE), 4) == -EAGAIN && ring.bufs == 0 &&
              write_wakes == 1,
          "a read lets go of an empty buffer and is eagain");
    CHECK(pipe_poll(&reader, PIPE_POLLIN, NULL) == 0, "a pipe whose empty buffer went polls empty");

    setup(0);
    pipe_ring_put(&ring, memory, 6);
    pages[1] = 0;
    peer_count = 4;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(PAGE_SIZE - 2), 6) == 6,
          "a read keeps its buffer across a later page's resolve");
    CHECK(pipe_available(&ring) == 0 && waits == 0 && peer_result == -EAGAIN,
          "a peer's read after it finds the ring drained");

    setup(0);
    pages[1] = 0;
    resolve_action = close_reader;
    CHECK(pipe_write(&writer, user_at(PAGE_SIZE - 2), 6) == -EPIPE,
          "a close during the group's resolve returns epipe");
    CHECK(signals == 1 && pipe_available(&ring) == 0,
          "a close during the group's resolve posts the signal and adds no byte");
}

/** Run every case; 0 when each passes, 1 otherwise. */
int main(void)
{
    read_competition(0, 0);
    read_competition(PIPE_BUF_SIZE - 2, 1);
    read_competition(0xfffffffeu, 0);
    write_competition(0);
    write_competition(1);
    state_changes();
    readiness_changes();
    write_with_data();
    later_page();
    fault_holds_pipe();
    printf("pipe-copy-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
