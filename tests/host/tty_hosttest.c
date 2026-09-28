/**
 * Exercise terminal waits with the production PTY, tty and wait-queue code.
 *
 * host-deps: rach2/sys/tty/n_tty.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The scheduler runs a producer only after the consumer is blocked. It checks
 * the wake before allowing the consumer to resume. There is no idle fallback,
 * timeout or endpoint close to make a missing wake appear successful.
 * User copies fail on a selected chunk; serial input and interrupt flags are
 * substituted. This tests queue transitions, not paging or interrupt delivery.
 *
 * Each wake of a pair names its kind of change for an edge-triggered epoll
 * registration, and the queues count them: input written to the master is data
 * for the slave, and its echo data for the master, or no change with echo off;
 * a slave read and an input flush are room for the master, and so is input the
 * master writes that takes bytes out of the slave's discipline, a signal
 * char's flush or an erase; and slave output is data for it.
 */
#define WAIT_QUEUE_HOSTTEST
#define wq_wait_event model_wait_event
#include "proc/wait_queue.c"
#undef wq_wait_event
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg);
#include "tty/tty.c"
#include "tty/pty.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

process_t *current_proc;
process_t *proc_list;
static process_t consumer, producer, poller;
static struct pty pair;
static fd_entry_t master, slave;
static int failures, irq_enabled = 1, pending_signal, switches;
static int recording, recorded_waits, read_stamps, serial_pos, serial_len;
static int copy_calls, copy_failure;
static const char *serial_bytes;
static void (*scheduled_action)(void);
static const char *wake_detail;
static char bytes[512];

#define CHECK(condition, detail)                        \
    do {                                                \
        if (!(condition)) {                             \
            dprintf(2, "tty-test: fail: %s\n", detail); \
            failures++;                                 \
        }                                               \
    } while (0)

/** Clear a fixture without using the kernel's libc aliases. */
static void clear(void *ptr, unsigned long size)
{
    unsigned char *p = ptr;

    while (size--) {
        *p++ = 0;
    }
}

/** Model the saved interrupt-enable flag, including nested regions. */
uint32_t kirq_save(void)
{
    uint32_t saved = (uint32_t)irq_enabled;

    irq_enabled = 0;
    return saved;
}

/** Restore exactly the flag saved by the caller. */
void kirq_restore(uint32_t flags)
{
    irq_enabled = (int)flags;
}

/** Supply interrupt masking around the host queue's assembly-free wait. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    uint32_t flags = kirq_save();
    int rc = model_wait_event(wq, cond, arg);

    kirq_restore(flags);
    return rc;
}

/** Run another process once and require a wake before resuming the waiter. */
void schedule(void)
{
    process_t *waiting = current_proc;
    void (*action)(void) = scheduled_action;
    uint32_t flags = kirq_save();

    CHECK(waiting->state == PROC_BLOCKED, "consumer blocks before producer runs");
    CHECK(producer.state == PROC_READY, "another process remains runnable");
    CHECK(action != NULL, "wait has a scheduled producer");
    switches++;
    scheduled_action = NULL;
    current_proc = &producer;
    if (action) {
        action();
    }
    current_proc = waiting;
    CHECK(waiting->state == PROC_READY, wake_detail);
    if (waiting->state != PROC_READY) {
        pending_signal = 1;
    }
    kirq_restore(flags);
}

/** End a failed modeled wait instead of repeating it indefinitely. */
int signal_pending(void)
{
    return pending_signal;
}

/** No row sends a terminal signal. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    (void)signo;
    CHECK(0, "no signal substitutes for a readiness wake");
}

/** Supply one zeroed allocation for the real PTY allocation path. */
void *kzalloc(size_t size)
{
    CHECK(size == sizeof(pair), "allocation is one terminal pair");
    clear(&pair, sizeof(pair));
    return &pair;
}

/** The fixture owns the static pair. */
void kfree(void *p)
{
    CHECK(p == &pair, "free refers to the terminal pair");
}

/** Descriptor allocation is outside these file-operation tests. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    CHECK(0, "file operations do not allocate a descriptor");
    return -1;
}

/** Host addresses are accepted; copy failures are injected separately. */
int user_range_ok(uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
    return 1;
}

/** Copy an ordinary chunk or fail the selected copy before transferring it. */
int copy_from_user(void *dst, const void *src, uint32_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    copy_calls++;
    if (copy_calls == copy_failure) {
        return -EFAULT;
    }
    while (n--) {
        *d++ = *s++;
    }
    return 0;
}

/** Return the bytes actually consumed by the production read. */
int copy_to_user(void *dst, const void *src, uint32_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return 0;
}

/** Use a nonzero timestamp without imposing a timing assertion. */
uint32_t pit_now_us(void)
{
    return 1;
}

/** Latency values are outside the readiness tests. */
void latency_span(int kind, uint32_t from, uint32_t to)
{
    (void)kind;
    (void)from;
    (void)to;
}

/** Select the production latency-recording branch. */
int usb_stamp_recording(void)
{
    return recording;
}

/** Count completed console reads. */
void usb_stamp_read(void)
{
    read_stamps++;
}

/** Count the empty-input latency record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    recorded_waits++;
}

/** Console output is irrelevant to the serial-input fixture. */
void console_putc_raw(char c)
{
    (void)c;
}

/** No console writes need batching in the fixture. */
void console_write_begin(void)
{}

/** Finish the fixture's empty output batch. */
void console_write_end(void)
{}

/** Check the interrupt lock at the first operation of each console pump. */
uint32_t serial_rx_pending(void)
{
    CHECK(!irq_enabled, "console pump excludes input interrupts");
    return (uint32_t)(serial_len - serial_pos);
}

/** Supply each serial byte once and require the drain to remain locked. */
int serial_rx_take(void)
{
    CHECK(!irq_enabled, "serial drain excludes input interrupts");
    return serial_pos < serial_len ? serial_bytes[serial_pos++] : -1;
}

/** Serial availability is independent of any process state. */
int serial_data_available(void)
{
    return serial_pos < serial_len;
}

/** The fixture injects complete UART transfers. */
int serial_rx_service(int budget)
{
    (void)budget;
    return 0;
}

/** No injected serial input contains a signal character. */
int serial_rx_find(const unsigned char *set, int n)
{
    (void)set;
    (void)n;
    return -1;
}

/** Advance the simulated serial ring. */
void serial_rx_discard(int count)
{
    serial_pos += count;
}

/** Empty simulated serial input on a console flush. */
void serial_rx_flush(void)
{
    serial_pos = serial_len;
}

/** Counters are unused by these rows. */
void serial_rx_counters(struct serial_rx_counters *out)
{
    clear(out, sizeof(*out));
}

/** The test invokes the interrupt notification explicitly. */
void serial_set_rx_notify(void (*notify)(void))
{
    (void)notify;
}

/** Initialize open endpoints and distinct consumer, producer and poller. */
static void setup_pair(void)
{
    int i;

    clear(&consumer, sizeof(consumer));
    clear(&producer, sizeof(producer));
    clear(&poller, sizeof(poller));
    consumer.state = PROC_RUNNING;
    consumer.waiter.proc = &consumer;
    producer.state = PROC_READY;
    producer.pid = 2;
    producer.sid = 1;
    poller.waiter.proc = &poller;
    current_proc = &consumer;
    pending_signal = switches = recording = copy_calls = copy_failure = 0;
    irq_enabled = 1;
    pty_init_pool();
    CHECK(pty_alloc() == &pair, "production allocation initializes the pair");
    pair.master_open = pair.slave_opens = 1;
    pair.slave.ldisc.tio.c_lflag &= ~(ICANON | ECHO | ISIG);
    clear(&master, sizeof(master));
    clear(&slave, sizeof(slave));
    master.priv = slave.priv = &pair;
    master.flags = slave.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    for (i = 0; i < (int)sizeof(bytes); i++) {
        bytes[i] = 'x';
    }
}

/** Fill through the real master writer until its poll reports no space. */
static void fill_input(void)
{
    int i;
    wait_queue_t *wq = NULL;

    for (i = 0; i < N_TTY_READ_MAX && n_tty_has_room(&pair.slave.ldisc); i++) {
        CHECK(ptm_write(&master, "x", 1) == 1, "master fills slave input");
    }
    CHECK(ptm_poll(&master, POLLOUT, &wq) == 0, "full input prevents POLLOUT");
}

/** Register exactly the queue returned by a master POLLOUT query. */
static wait_queue_t *park_poller(void)
{
    wait_queue_t *wq = NULL;

    CHECK(ptm_poll(&master, POLLOUT, &wq) == 0, "poller has no ready event");
    CHECK(wq != NULL, "master poll supplies a queue");
    poller.state = PROC_BLOCKED;
    wq_poll_add(wq, &poller.waiter);
    return wq;
}

/** A slave read must wake the queue returned to a blocked master poll. */
static void read_wakes_poll(void)
{
    wait_queue_t *wq;
    char byte;

    setup_pair();
    fill_input();
    wq = park_poller();
    current_proc = &producer;
    CHECK(pts_read(&slave, &byte, 1) == 1 && byte == 'x', "slave consumes input");
    CHECK(poller.state == PROC_READY, "slave read wakes master POLLOUT");
    CHECK(ptm_poll(&master, POLLOUT, &wq) == POLLOUT, "slave read creates space");
    wq_poll_remove(wq, &poller.waiter);
}

static int flush_termios;

/** Free input only after the writer has registered its wait. */
static void flush_action(void)
{
    wait_queue_t *wq = park_poller();
    struct ktermios tio = pair.slave.ldisc.tio;

    if (flush_termios) {
        tty_set_termios(&pair.slave, &tio, 1);
    } else {
        tty_flush_input(&pair.slave);
    }
    CHECK(poller.state == PROC_READY, "input flush wakes master POLLOUT");
    CHECK(ptm_poll(&master, POLLOUT, &wq) == POLLOUT, "input flush creates space");
    wq_poll_remove(wq, &poller.waiter);
}

/** Both tty flush entry points must release a blocked master writer. */
static void flush_wakes_writer(int termios)
{
    setup_pair();
    fill_input();
    flush_termios = termios;
    scheduled_action = flush_action;
    wake_detail = termios ? "TCSETSF wakes master writer" : "TCFLSH wakes master writer";
    CHECK(ptm_write(&master, "z", 1) == 1, "master write resumes after flush");
    CHECK(switches == 1, "master write blocked exactly once");
    CHECK(pair.master_open && pair.slave_opens, "flush leaves both endpoints open");
    CHECK(n_tty_inq(&pair.slave.ldisc) == 1, "only post-flush input remains");
}

static int echo_write;

/** Queue one chunk, then fail the next copy while a master reader waits. */
static void partial_action(void)
{
    int rc;

    CHECK(pty_m_avail(&pair) == 0, "master reader parked before output");
    copy_failure = 2;
    rc = echo_write ? ptm_write(&master, bytes, sizeof(bytes))
                    : pts_write(&slave, bytes, sizeof(bytes));
    CHECK(copy_calls == 2 && rc == 256, "second copy fails after one accepted chunk");
    CHECK(pty_m_avail(&pair) == 256, "partial write leaves 256 output bytes");
}

/** A partial output or echo write must release a blocked master reader. */
static void partial_wakes_reader(int echo)
{
    char out[512];
    int i, n;

    setup_pair();
    echo_write = echo;
    if (echo) {
        pair.slave.ldisc.tio.c_lflag |= ECHO;
    }
    scheduled_action = partial_action;
    wake_detail = echo ? "partial master echo wakes reader" : "partial slave output wakes reader";
    n = ptm_read(&master, out, sizeof(out));
    CHECK(n == 256, "master reads exactly the accepted chunk");
    for (i = 0; i < n; i++) {
        CHECK(out[i] == 'x', "partial write preserves every byte");
    }
    CHECK(switches == 1, "master reader blocked exactly once");
    CHECK(pair.master_open && pair.slave_opens, "partial write leaves endpoints open");
}

/** Simulate input notification while the console reader is parked. */
static void serial_action(void)
{
    CHECK(wq_active(&console_tty.input_wq), "console reader registered before input");
    serial_bytes = "q\n";
    serial_len = 2;
    tty_console_poll();
}

/** The latency predicate must mask interrupts even before a waiter exists. */
static void latency_read(void)
{
    unsigned char out[4];

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1);
    recording = 1;
    recorded_waits = read_stamps = serial_pos = serial_len = 0;
    scheduled_action = serial_action;
    wake_detail = "serial notification wakes console reader";
    CHECK(!wq_active(&console_tty.input_wq), "latency check starts without a waiter");
    CHECK(tty_read(&console_tty, out, sizeof(out), 0) == 2, "console read completes");
    CHECK(out[0] == 'q' && out[1] == '\n', "console input is intact");
    CHECK(recorded_waits == 1 && read_stamps == 1, "latency path records wait and read");
    CHECK(switches == 1 && irq_enabled, "console read restores interrupt state");
}

/** Each wake of a pair names the kind of change it is to the pollers of its
 * queue, and no other. */
static void wake_kinds(void)
{
    char byte;

    setup_pair();
    current_proc = &producer;
    CHECK(ptm_write(&master, "a", 1) == 1, "master writes input");
    CHECK(wq_wakes(&pair.slave.input_wq, WQ_DATA) == 1 &&
              wq_wakes(&pair.slave.input_wq, WQ_ROOM | WQ_HANGUP) == 0,
          "input written to the master is data for the slave");
    CHECK(wq_wakes(&pair.m_read_wq, WQ_ALL) == 0,
          "input written with echo off is no change for the master");
    pair.slave.ldisc.tio.c_lflag |= ECHO;
    CHECK(ptm_write(&master, "b", 1) == 1, "master writes echoed input");
    CHECK(wq_wakes(&pair.m_read_wq, WQ_DATA) == 1 && wq_wakes(&pair.m_read_wq, WQ_ROOM) == 0,
          "echo is data for the master");
    CHECK(pts_read(&slave, &byte, 1) == 1 && byte == 'a', "slave reads input");
    CHECK(wq_wakes(&pair.m_read_wq, WQ_ROOM) == 1 && wq_wakes(&pair.m_read_wq, WQ_DATA) == 1,
          "a slave read is room for the master and no data");
    CHECK(pts_write(&slave, "c", 1) == 1, "slave writes output");
    CHECK(wq_wakes(&pair.m_read_wq, WQ_DATA) == 2 && wq_wakes(&pair.m_read_wq, WQ_ROOM) == 1,
          "slave output is data for the master and no room");
    tty_flush_input(&pair.slave);
    CHECK(wq_wakes(&pair.m_read_wq, WQ_ROOM) == 2 && wq_wakes(&pair.m_read_wq, WQ_DATA) == 2,
          "an input flush is room for the master and no data");
}

/**
 * Input the master writes that takes bytes out of the slave's discipline is
 * room for the master: with the slave's input full, a VINTR written with
 * ISIG on and echo off flushes it and wakes a parked master poller for
 * room; in canonical mode an erase of a typed byte is room too.
 */
static void input_room(void)
{
    wait_queue_t *wq;
    uint32_t rooms;

    setup_pair();
    current_proc = &producer;
    pair.slave.ldisc.tio.c_lflag |= ISIG;
    fill_input();
    wq = park_poller();
    rooms = wq_wakes(&pair.m_read_wq, WQ_ROOM);
    CHECK(ptm_write(&master, "\003", 1) == 1, "the master writes VINTR into full input");
    CHECK(wq_wakes(&pair.m_read_wq, WQ_ROOM) == rooms + 1 && poller.state == PROC_READY,
          "a signal char the master writes into full input is room for the master");
    CHECK(ptm_poll(&master, POLLOUT, &wq) == POLLOUT, "the flush of a signal char makes room");
    wq_poll_remove(wq, &poller.waiter);
    pair.slave.ldisc.tio.c_lflag |= ICANON;
    CHECK(ptm_write(&master, "ab", 2) == 2, "the master types part of a line");
    rooms = wq_wakes(&pair.m_read_wq, WQ_ROOM);
    CHECK(ptm_write(&master, "\177", 1) == 1 && wq_wakes(&pair.m_read_wq, WQ_ROOM) == rooms + 1,
          "an erase the master writes is room for the master");
}

/** Run the independent wake and locking regressions. */
int main(void)
{
    read_wakes_poll();
    flush_wakes_writer(0);
    flush_wakes_writer(1);
    partial_wakes_reader(0);
    partial_wakes_reader(1);
    latency_read();
    wake_kinds();
    input_room();
    printf("tty-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}

/** The terminal test does not track a USB reader. */
void usb_stamp_wait(uint32_t pid)
{
    (void)pid;
}
