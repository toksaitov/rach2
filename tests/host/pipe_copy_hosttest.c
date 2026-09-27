/**
 * Run pipe transfers with another operation inside the page resolver.
 *
 * host-deps:
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * Pipe, ring and checked-copy code are the kernel's. User addresses refer to
 * model pages; the resolver runs a peer operation before returning a missing
 * or copy-on-write page. Waits, signals and wakeups are recorded substitutes;
 * a wakeup keeps the kinds of change it names, so a read must wake the writers
 * for room alone, a write the readers for data alone, and a close the other
 * end for a hangup.
 */
#include "ipc/pipe.c"
#include "ipc/pipe_ring.c"
#include "mm/usercopy.c"

extern int printf(const char *format, ...);

#define USER_BASE 0x04000000u
#define PEER_PAGE 4

static pipe_t ring;
static fd_entry_t reader, writer;
static struct open_file read_ofd, write_ofd;
static uint8_t memory[8 * PAGE_SIZE];
static int pages[8];
static int failures, waits, read_wakes, write_wakes, signals, resolves;
static uint32_t read_kinds, write_kinds;
static void (*resolve_action)(void);
static void (*wait_action)(void);
static uint32_t peer_count;
static int peer_closes;
static process_t writing_thread;
process_t *current_proc = &writing_thread;

#define CHECK(condition, detail)                                \
    do {                                                        \
        if (!(condition)) {                                     \
            printf("pipe-copy-hosttest: failed: %s\n", detail); \
            failures++;                                         \
        }                                                       \
    } while (0)

/** Return a model user address without accessing it on the host. */
static char *user_at(uint32_t offset)
{
    return (char *)(unsigned long)(USER_BASE + offset);
}

/** Translate model addresses only at the raw memory-copy boundary. */
static void *host_address(const void *ptr)
{
    unsigned long addr = (unsigned long)ptr;

    if (addr >= USER_BASE && addr < USER_BASE + sizeof(memory)) {
        return memory + (addr - USER_BASE);
    }
    return (void *)ptr;
}

/** Copy bytes after translating model user addresses. */
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

/** Clear host structures without linking the kernel's libc aliases. */
void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

/** Resolve one page, allowing a peer to run only when allocation is needed. */
int vm_prefault_range(uint32_t addr, uint32_t len, int write)
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

/** Model a pipe wait; an unconfigured wait is interrupted. */
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

/** Count readiness notifications, and the kinds of change they name,
 * without switching processes. */
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

/** Initialize queues for the unused pipe-creation path. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Record SIGPIPE from a writer whose last reader closed, which goes to the
 * writing thread. */
void signal_post_thread(process_t *proc, int signo, const struct k_siginfo *info)
{
    CHECK(proc == current_proc && signo == SIGPIPE && info->code == SI_USER,
          "writer posts sigpipe");
    signals++;
}

/** Pipe creation is outside this test. */
void *kzalloc(size_t n)
{
    (void)n;
    CHECK(0, "unexpected allocation");
    return NULL;
}

/** A tested transfer retains its own pipe end. */
void kfree(void *ptr)
{
    (void)ptr;
    CHECK(0, "unexpected pipe release");
}

/** Descriptor allocation is outside this test. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    CHECK(0, "unexpected descriptor allocation");
    return -1;
}

/** Descriptor release is outside this test. */
int fd_free(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    CHECK(0, "unexpected descriptor release");
    return -1;
}

/** Reset two nonblocking ends and eight writable model pages. */
static void setup(uint32_t head)
{
    unsigned i;

    k_memset(&ring, 0, sizeof(ring));
    k_memset(&reader, 0, sizeof(reader));
    k_memset(&writer, 0, sizeof(writer));
    k_memset(&read_ofd, 0, sizeof(read_ofd));
    k_memset(&write_ofd, 0, sizeof(write_ofd));
    k_memset(memory, 0x33, sizeof(memory));
    ring.head = ring.tail = head;
    ring.read_open = ring.write_open = 1;
    reader.priv = writer.priv = &ring;
    reader.flags = FD_FLAG_READ;
    writer.flags = FD_FLAG_WRITE;
    reader.ofd = &read_ofd;
    writer.ofd = &write_ofd;
    read_ofd.status_flags = write_ofd.status_flags = OFD_NONBLOCK;
    for (i = 0; i < 8; i++) {
        pages[i] = 2;
    }
    k_memset(memory + PEER_PAGE * PAGE_SIZE, 0x22, PAGE_SIZE);
    waits = read_wakes = write_wakes = signals = resolves = 0;
    read_kinds = write_kinds = 0;
    resolve_action = wait_action = NULL;
    peer_count = 0;
    peer_closes = 0;
}

/** Consume bytes through the production reader during a resolver wait. */
static void peer_read(void)
{
    CHECK(pipe_read(&reader, user_at(PEER_PAGE * PAGE_SIZE), peer_count) == (int32_t)peer_count,
          "peer read count");
    if (peer_closes) {
        pipe_close(&writer);
    }
}

/** Append bytes through the production writer during a resolver wait. */
static void peer_write(void)
{
    CHECK(pipe_write(&writer, user_at(PEER_PAGE * PAGE_SIZE), peer_count) == (int32_t)peer_count,
          "peer write count");
}

/** Close the other end while a writer resolves its source page. */
static void close_reader(void)
{
    pipe_close(&reader);
}

/** Check competing readers across physical and counter wrap, including COW. */
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

/** Check peer writes preserve byte order and cannot exceed ring capacity. */
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

/** Check empty/full rechecks, EOF, signals and readiness after a pipe wait. */
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
    pipe_ring_put(&ring, memory, PIPE_BUF_SIZE);
    CHECK(pipe_write(&writer, user_at(0), 1) == -EINTR, "full write interrupted");
    peer_count = PIPE_BUF_SIZE;
    wait_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "write rechecks after wait");
    CHECK(waits == 2 && read_wakes == 1, "refill wakes reader");
    CHECK(read_kinds == WQ_DATA, "a write wakes the readers for data alone");
    CHECK(pipe_ring_get(&ring, bytes, sizeof(bytes)) == 3, "refill count");
}

/** A resolver can remove readiness before either blocking operation copies. */
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
    CHECK(read_wakes == 2, "each refill wakes reader");

    setup(0);
    pipe_ring_put(&ring, memory, 2);
    pages[0] = 0;
    peer_count = 2;
    resolve_action = peer_read;
    CHECK(pipe_write(&writer, user_at(0), 3) == 3, "writer copies after peer drains");
    CHECK(read_wakes == 1, "writer rechecks empty transition after resolution");
}

/** A second page fault or competing read returns only completed bytes. */
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

/** Run deterministic scheduling cases and report one process exit status. */
int main(void)
{
    read_competition(0, 0);
    read_competition(PIPE_BUF_SIZE - 2, 1);
    read_competition(0xfffffffeu, 0);
    write_competition(0);
    write_competition(1);
    state_changes();
    readiness_changes();
    later_page();
    printf("pipe-copy-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
