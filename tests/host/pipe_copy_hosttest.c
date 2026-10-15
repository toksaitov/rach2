/**
 * Host test of pipe transfers in which another pipe operation runs while a
 * user page is resolved.
 *
 * host-deps:
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * Pipe, ring and checked-copy code are the kernel's. User addresses refer to
 * model pages; the resolver runs a peer operation before returning a missing
 * or copy-on-write page. Waits, signals and wakeups are recorded substitutes;
 * a wakeup keeps the kinds of change it names, so a read must wake the writers
 * for room alone, a write the readers for data alone, whether the ring had
 * data before it or not, and a close the other end for a hangup.
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
static fd_entry_t reader, writer;
static struct open_file read_ofd, write_ofd;
static uint8_t memory[USER_PAGES * PAGE_SIZE];
static int pages[USER_PAGES];
static int failures, waits, read_wakes, write_wakes, signals, resolves;
static uint32_t read_kinds, write_kinds;
static void (*resolve_action)(void);
static void (*wait_action)(void);
static uint32_t peer_count;
static int peer_closes;
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

/**
 * Resolve the page of `addr` for a read or, with `write`, a write: 0, a
 * fault, for an address outside the model or a page marked -1, and 1
 * otherwise. A page whose state is below what the access needs (0 missing, 1
 * copy-on-write, 2 writable) first runs `resolve_action` once, as a peer
 * would run while the kernel allocates the frame. A copy must stay in one
 * page.
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
            action();
        }
        pages[page] = write ? 2 : 1;
    }
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
    action();
    CHECK(cond(arg), "peer makes wait ready");
    return 0;
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
    peer_closes = 0;
}

/** Read `peer_count` bytes through `pipe_read`, as a peer during a resolve
 * or a wait, and with `peer_closes` close the writer after. */
static void peer_read(void)
{
    CHECK(pipe_read(&reader, user_at(PEER_PAGE * PAGE_SIZE), peer_count) == (int32_t)peer_count,
          "peer read count");
    if (peer_closes) {
        pipe_close(&writer);
    }
}

/** Write `peer_count` bytes through `pipe_write`, as a peer during a
 * resolve or a wait. */
static void peer_write(void)
{
    CHECK(pipe_write(&writer, user_at(PEER_PAGE * PAGE_SIZE), peer_count) == (int32_t)peer_count,
          "peer write count");
}

/** Close the read end, as a peer while a writer resolves its source page. */
static void close_reader(void)
{
    pipe_close(&reader);
}

/**
 * A reader whose page is resolved while a peer reads 2 of the 6 bytes
 * copies the other 4 from the head as it is after the peer, and consumes no
 * byte twice. `head` is the ring's head: 0, at the end of the buffer, or at
 * the wrap of the counter; `present` is the page's state, 0 missing or 1
 * copy-on-write.
 */
static void read_competition(uint32_t head, int present)
{
    static const uint8_t bytes[] = {1, 2, 3, 4, 5, 6};

    setup(head);
    pipe_ring_put(&ring, bytes, sizeof(bytes));
    pages[0] = present;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(0), 6) == 4, "reader returns remaining bytes");
    CHECK(memory[0] == 3 && memory[1] == 4 && memory[2] == 5 && memory[3] == 6,
          "reader copies current head");
    CHECK(pipe_available(&ring) == 0, "reader does not consume twice");
    CHECK(resolves == 1, "reader resolves lazy or cow page once");
}

/**
 * A writer whose page is resolved while a peer writes rechecks the free
 * space: with `fill` the peer fills the ring and the write gives -EAGAIN;
 * without, the writer's 3 bytes follow the peer's 4. No byte already in the
 * ring is lost.
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
    peer_count = fill ? PIPE_BUF_SIZE - 2 : 4;
    resolve_action = peer_write;
    result = pipe_write(&writer, user_at(0), 3);
    CHECK(result == (fill ? -EAGAIN : 3), "writer rechecks free space");
    CHECK(pipe_available(&ring) == 2 + peer_count + (fill ? 0 : 3),
          "writer occupancy stays bounded");
    pipe_ring_get(&ring, bytes, PIPE_BUF_SIZE);
    CHECK(bytes[0] == 0x11 && bytes[1] == 0x11, "writer preserves unread bytes");
    for (i = 0; i < peer_count; i++) {
        if (bytes[2 + i] != 0x22) {
            intact = 0;
        }
    }
    CHECK(intact, "writer preserves peer bytes");
    if (!fill) {
        CHECK(bytes[6] == 0x33 && bytes[8] == 0x33, "writer appends after peer");
    }
    CHECK(resolves == 1, "writer resolves lazy page once");
}

/**
 * A transfer rechecks the ring after a resolve or a wait: a read of a ring a
 * peer drained gives -EAGAIN, or 0 once the writer closed; a write after the
 * reader closed gives -EPIPE and SIGPIPE; a blocking read or write waits,
 * and a read wakes the writers for room alone and a write the readers for
 * data alone.
 */
static void state_changes(void)
{
    uint8_t bytes[PIPE_BUF_SIZE];

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(0), 2) == -EAGAIN, "drained reader returns eagain");
    CHECK(pipe_available(&ring) == 0, "drained ring stays empty");

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    peer_closes = 1;
    resolve_action = peer_read;
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
 * A blocking reader whose ring a peer drains during the resolve waits for
 * new bytes, and a blocking writer whose ring a peer fills waits for room;
 * each page a write copies wakes the readers.
 */
static void readiness_changes(void)
{
    setup(0);
    read_ofd.status_flags = 0;
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    wait_action = peer_write;
    CHECK(pipe_read(&reader, user_at(0), 2) == 2, "drained blocking reader retries");
    CHECK(waits == 1 && pipe_available(&ring) == 0, "drained blocking reader waits for new bytes");

    setup(0);
    write_ofd.status_flags = 0;
    pages[0] = 0;
    peer_count = PIPE_BUF_SIZE;
    resolve_action = peer_write;
    wait_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "filled blocking writer retries");
    CHECK(waits == 1 && pipe_available(&ring) == 3, "filled blocking writer waits for space");
    CHECK(read_wakes == PIPE_BUF_SIZE / PAGE_SIZE + 1, "each page a write copies wakes reader");

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "writer copies after peer drains");
    CHECK(read_wakes == 1, "a write after a peer drained the ring wakes the readers once");
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

/** A transfer over two pages whose second faults, or whose resolve lets a
 * peer drain the ring or close the reader, returns the bytes of the first. */
static void later_page(void)
{
    setup(0);
    pipe_ring_put(&ring, memory, 6);
    pages[1] = -1;
    CHECK(pipe_read(&reader, user_at(PAGE_SIZE - 2), 6) == 2, "read fault returns partial count");
    CHECK(pipe_available(&ring) == 4 && write_wakes == 1,
          "read fault leaves bytes and wakes writer");

    setup(0);
    pages[1] = -1;
    CHECK(pipe_write(&writer, user_at(PAGE_SIZE - 2), 6) == 2, "write fault returns partial count");
    CHECK(pipe_available(&ring) == 2 && read_wakes == 1, "write fault publishes completed bytes");

    setup(0);
    pipe_ring_put(&ring, memory, 6);
    pages[1] = 0;
    peer_count = 4;
    resolve_action = peer_read;
    CHECK(pipe_read(&reader, user_at(PAGE_SIZE - 2), 6) == 2, "later drain returns partial count");
    CHECK(pipe_available(&ring) == 0 && waits == 0, "partial reader does not wait for more");

    setup(0);
    pages[1] = 0;
    resolve_action = close_reader;
    CHECK(pipe_write(&writer, user_at(PAGE_SIZE - 2), 6) == 2, "later close returns partial count");
    CHECK(signals == 1 && pipe_available(&ring) == 2, "later close posts signal without copying");
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
    printf("pipe-copy-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
