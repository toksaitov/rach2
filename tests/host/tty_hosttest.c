/**
 * Host test of a terminal's waits and wakeups, in tty/tty.c, tty/pty.c and
 * proc/wait_queue.c, over the kernel's line discipline.
 *
 * host-deps: rach2/sys/tty/n_tty.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The test's `schedule` runs a producer only after the consumer has blocked,
 * and checks the wake before the consumer resumes; no idle loop, time limit
 * or close of an end can hide a missing wake. A chosen copy from the user
 * fails, and the serial input and the interrupt flag are the test's. The
 * rows test the queues' changes of state, not paging or the delivery of
 * interrupts.
 *
 * Each wake of a pair names its kind of change for an edge-triggered epoll
 * registration, and the queues count them: input written to the master is data
 * for the slave, and its echo data for the master, or no change with echo off;
 * a slave read and an input flush are room for the master, and so is input the
 * master writes that takes bytes out of the slave's discipline, a signal
 * char's flush or an erase; and slave output is data for it. In packet mode
 * a master read begins with the status byte, and TCFLSH on the master drops
 * the slave's output. A read that does not wait ends the console latency
 * span.
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

struct percpu cpu_table[NR_CPUS];
process_t *proc_list;
static process_t consumer, producer, poller;
static struct pty pair;
static fd_entry_t master, slave;
static int failures, irq_enabled = 1, pending_signal, switches;
static int recording, recorded_waits, read_stamps, serial_pos, serial_len;
static int copy_calls, copy_failure, spans;
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

/** Zero `size` bytes at `ptr`, so that the test links none of the kernel's
 * string functions. */
static void clear(void *ptr, unsigned long size)
{
    unsigned char *p = ptr;

    while (size--) {
        *p++ = 0;
    }
}

/** Clear the model's interrupt flag and return what it was. */
uint32_t kirq_save(void)
{
    uint32_t saved = (uint32_t)irq_enabled;

    irq_enabled = 0;
    return saved;
}

/** Set the model's interrupt flag to `flags`. */
void kirq_restore(uint32_t flags)
{
    irq_enabled = (int)flags;
}

/** The wait queue's wait, with interrupts off around it as the kernel's
 * wait has them. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    uint32_t flags = kirq_save();
    int rc = model_wait_event(wq, cond, arg);

    kirq_restore(flags);
    return rc;
}

/** Run `scheduled_action` once as the producer, with the consumer blocked,
 * and check that it woke the consumer; when it did not, make a signal
 * pending so that the wait ends. */
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

/** Whether `schedule` found a missing wake, which ends the wait. */
int signal_pending(void)
{
    return pending_signal;
}

/** A failure: no row sends a terminal signal. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    (void)signo;
    CHECK(0, "no signal substitutes for a readiness wake");
}

/** The test's pair, zeroed, for `pty_alloc`; a request of another size is
 * a failure. */
void *kzalloc(size_t size)
{
    CHECK(size == sizeof(pair), "allocation is one terminal pair");
    clear(&pair, sizeof(pair));
    return &pair;
}

/** Check that `p` is the test's pair, which is static. */
void kfree(void *p)
{
    CHECK(p == &pair, "free refers to the terminal pair");
}

/** A failure: no row allocates a descriptor. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    CHECK(0, "file operations do not allocate a descriptor");
    return -1;
}

/** 1: every host address is user memory. A copy fails only as
 * `copy_failure` chooses. */
int user_range_ok(uintptr_t addr, size_t len)
{
    (void)addr;
    (void)len;
    return 1;
}

/** Copy `n` bytes from the host address `src`; -EFAULT, copying nothing,
 * when this is copy number `copy_failure`. */
int copy_from_user(void *dst, const void __user *src, size_t n)
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

/** Copy `n` bytes to the host address `dst`; 0. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return 0;
}

/** 1, a time that is not 0; no row judges a time. */
uint32_t timer_now_us(void)
{
    return 1;
}

/** 1 s: no row reads a pty's times. */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

/** Count the console latency spans recorded. */
void latency_span(int kind, uint32_t from, uint32_t to)
{
    (void)from;
    (void)to;
    if (kind == LAT_CONSOLE) {
        spans++;
    }
}

/** Whether the console's latency is recorded: `recording`. */
int usb_stamp_recording(void)
{
    return recording;
}

/** Count completed console reads. */
void usb_stamp_read(void)
{
    read_stamps++;
}

/** Count a record; a console read that waits for input with latency
 * recorded logs one. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    recorded_waits++;
}

/** Nothing: no row reads the console's output. */
void console_putc_raw(char c)
{
    (void)c;
}

/** The fixture's screen answers no query. */
unsigned int k_take_answer(char *buf, unsigned int size)
{
    (void)buf;
    (void)size;
    return 0;
}

/** Nothing: the console's output is not batched. */
void console_write_begin(void)
{}

/** Nothing: the console's output is not batched. */
void console_write_end(void)
{}

/** The console's cells: 85 by 24, as a 1366x768 screen at twice the font
 * has them, and not the 80 by 25 a terminal would take by default. */
void k_console_size(unsigned int *cols, unsigned int *rows)
{
    *cols = 85;
    *rows = 24;
}

/** The serial bytes not yet taken; check that interrupts are off, as the
 * console's pump has them. */
uint32_t serial_rx_pending(void)
{
    CHECK(!irq_enabled, "console pump excludes input interrupts");
    return (uint32_t)(serial_len - serial_pos);
}

/** The next serial byte, or -1 when none is left; check that interrupts
 * are off. */
int serial_rx_take(void)
{
    CHECK(!irq_enabled, "serial drain excludes input interrupts");
    return serial_pos < serial_len ? serial_bytes[serial_pos++] : -1;
}

/** Whether a serial byte is left. */
int serial_data_available(void)
{
    return serial_pos < serial_len;
}

/** 0: the test puts whole transfers in the serial input. */
int serial_rx_service(int budget)
{
    (void)budget;
    return 0;
}

/** -1: no serial input has a signal character. */
int serial_rx_find(const unsigned char *set, int n)
{
    (void)set;
    (void)n;
    return -1;
}

/** Drop `count` serial bytes. */
void serial_rx_discard(int count)
{
    serial_pos += count;
}

/** Drop every serial byte, for a flush of the console's input. */
void serial_rx_flush(void)
{
    serial_pos = serial_len;
}

/** Zeroed counters: no row reads them. */
void serial_rx_counters(struct serial_rx_counters *out)
{
    clear(out, sizeof(*out));
}

/** Nothing: a row calls `tty_console_poll` itself. */
void serial_set_rx_notify(void (*notify)(void))
{
    (void)notify;
}

/** Reset the consumer, current and running, the producer, ready, and the
 * poller, and allocate the pair with both ends open in raw mode without
 * echo or signals. */
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
    pending_signal = switches = recording = copy_calls = copy_failure = spans = 0;
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

/** Fill the slave's input through `ptm_write` until the master's poll
 * reports no room. */
static void fill_input(void)
{
    int i;
    wait_queue_t *wq = NULL;

    for (i = 0; i < N_TTY_READ_MAX && n_tty_has_room(&pair.slave.ldisc); i++) {
        CHECK(ptm_write(&master, "x", 1) == 1, "master fills slave input");
    }
    CHECK(ptm_poll(&master, POLLOUT, &wq) == 0, "full input prevents POLLOUT");
}

/** Block the poller on the queue the master's poll for POLLOUT returns,
 * and return that queue. */
static wait_queue_t *park_poller(void)
{
    wait_queue_t *wq = NULL;

    CHECK(ptm_poll(&master, POLLOUT, &wq) == 0, "poller has no ready event");
    CHECK(wq != NULL, "master poll supplies a queue");
    poller.state = PROC_BLOCKED;
    wq_poll_add(wq, &poller.waiter);
    return wq;
}

/** A slave read from full input wakes a poller blocked for the master's
 * POLLOUT. */
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

/** With the master writer blocked, flush the slave's input, by TCSETSF
 * when `flush_termios` is set and by TCFLSH otherwise, and check that a
 * blocked poller wakes and the master has room. */
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

/** A master writer blocked on full input resumes after a flush, by TCSETSF
 * with `termios` and by TCFLSH without, and only its byte is left. */
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

/** With the master reader blocked, write 512 bytes from the slave, or from
 * the master with echo when `echo_write` is set, whose second copy of 256
 * fails. */
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

/** A write of slave output, or of echoed master input with `echo`, that
 * fails after its first 256 bytes wakes the blocked master reader, which
 * reads those 256. */
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

/** With the console reader blocked, give the serial input "q\n" and poll
 * the console, as the serial interrupt does. */
static void serial_action(void)
{
    CHECK(wq_active(&console_tty.input_wq), "console reader registered before input");
    serial_bytes = "q\n";
    serial_len = 2;
    tty_console_poll();
}

/**
 * A console read with latency recorded waits for serial input and reads
 * it whole, logs one wait and stamps one read, and leaves interrupts on;
 * the console's window is its cells.
 */
static void latency_read(void)
{
    unsigned char out[4];

    setup_pair();
    tty_console_init();
    CHECK(console_tty.winsize.ws_col == 85 && console_tty.winsize.ws_row == 24,
          "the console's window is the console's cells");
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

/** Packet mode, which TIOCPKT sets: a master read gives TIOCPKT_DATA, 0,
 * before the slave's output; TCFLSH 0 on the master drops that output. The
 * ioctls' int travels through a 32-bit argument the host's pointers do not
 * fit in, so the row sets the mode and calls the flush itself. */
static void packet_mode(void)
{
    char out[8];

    setup_pair();
    scheduled_action = NULL;
    pair.packet = 1;
    CHECK(pts_write(&slave, "ab", 2) == 2, "the slave writes two bytes");
    CHECK(ptm_read(&master, out, sizeof(out)) == 3 && out[0] == 0 && out[1] == 'a' && out[2] == 'b',
          "a packet read begins with TIOCPKT_DATA");
    CHECK(pts_write(&slave, "c", 1) == 1 && ptm_flush(&pair, 0) == 0 && pty_m_avail(&pair) == 0,
          "TCFLSH 0 drops the slave's output");
    CHECK(ptm_flush(&pair, 3) == -EINVAL, "TCFLSH takes 0, 1 and 2");
}

/** A read that does not wait ends the latency span its input started, as a
 * read that waits does, so the next span starts from later input. */
static void nonblocking_latency(void)
{
    unsigned char out[4];

    setup_pair();
    current_proc = &producer;
    CHECK(ptm_write(&master, "a", 1) == 1 && pair.slave.ready_us != 0,
          "input written to the master starts a latency span");
    CHECK(tty_read(&pair.slave, out, sizeof(out), 1) == 1 && out[0] == 'a',
          "a read that does not wait takes the input");
    CHECK(spans == 1 && pair.slave.ready_us == 0,
          "a read that does not wait ends the latency span");
}

/** Run every row; 0 when each passes, 1 otherwise. */
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
    packet_mode();
    nonblocking_latency();
    printf("tty-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}

/** Nothing: no row checks the stamps of a USB reader. */
void usb_stamp_wait(uint32_t pid)
{
    (void)pid;
}
