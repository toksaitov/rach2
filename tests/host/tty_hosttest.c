/**
 * Host test of a terminal's waits and wakeups, in tty/tty.c, tty/pty.c and
 * proc/wait_queue.c, over the kernel's line discipline.
 *
 * host-deps: rach2/sys/tty/n_tty.c rach2/sys/wcwidth.c
 * host-deps: tests/host/compat/fd_flat.c tests/host/compat/bkl_held.c
 * host-deps: tests/host/compat/atomic_host.c
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
 * a master read begins with the status byte, TCFLSH on the master drops the
 * slave's output, a slave's flush gives the master FLUSHREAD and FLUSHWRITE,
 * and a vhangup leaves it no status. A read that does not wait ends the
 * console latency span, and slave output takes no more of the master's ring
 * than it needs.
 *
 * While the console's keyboard is in a raw mode, COM1's bytes are discarded,
 * by the tick's drain and by a reader's, and none reaches the discipline;
 * once it translates again, they are read as before.
 *
 * A noncanonical read waits by POSIX's four cases of VMIN and VTIME, on a
 * slave and on the console, under a model tick: each row's steps give the
 * tick, the input that arrives then or the timer's wake, and the deadline the
 * reader must have armed in `wakeup_tick` by then. A slave reader whose
 * master closes returns 0, and so does each read after it.
 *
 * Output flow is Linux 6.12's: VSTOP from the master stops the slave's
 * output, a slave writer waits until VSTART wakes it, its POLLOUT goes, and
 * a master in packet mode reads TIOCPKT_STOP and TIOCPKT_START; the echo
 * waits while output is stopped; TCOOFF holds against VSTART until TCOON,
 * and TCIOFF and TCION write their characters; IXON with ^S and ^Q turned
 * off and on gives TIOCPKT_NOSTOP and TIOCPKT_DOSTOP, and off restarts
 * output; a signal character discards the slave's output and reports
 * TIOCPKT_FLUSHREAD and TIOCPKT_FLUSHWRITE before its echo, and under NOFLSH
 * discards nothing; a VSTOP behind a full slave acts at once and not again
 * when fed; a master stopped by TCOOFF waits; and on the console a writer
 * waits for a VSTART from COM1, a VSTOP behind a full console acts at once,
 * and Hold and the Scroll Lock flag follow the stop.
 *
 * The echo held goes out ahead of a slave write only as far as the master's
 * ring has room, and the write waits for the rest instead of losing its
 * bytes; a master write goes on after the last slave closes; a VSTART from
 * COM1 drained with no reader keeps the keyboard echo held during the stop
 * for the next write, and a byte that drain takes behind a full buffer of it
 * holds its echo there, the oldest byte giving way;
 * a nonblocking master write behind a full slave counts the flow characters
 * it acts on, and its VSTART gives out the echo held, and writes of data
 * that end in a flow character feed no more than the read queue stores and
 * count only what they feed; a background
 * process's change of its terminal takes SIGTTOU unless it ignores it, its
 * read EIO when it ignores SIGTTIN, and an orphaned group's call EIO, a
 * parent whose leader exited while a thread runs keeping the group joined,
 * while a TOSTOP write that ignores SIGTTOU goes ahead; the console's TCIOFF
 * writes its character under TCOOFF; and a session leader's read of a slave
 * does not make it its controlling terminal.
 *
 * At the end of the console's session leader, a reader asleep across it
 * takes none of the next session's line though that line came before the
 * reader ran, nor in the rest of a line it reads after its copy; a writer
 * whose copy from the user, service point or wait for stopped output spans
 * it writes nothing more; and a poller that asked for no events is woken,
 * and its description's poll then gives POLLHUP. Serial input, a Ctrl-C
 * and a Ctrl-Z that come while the console's only waiter is such a poller,
 * or an epoll registration, reach the discipline and the foreground group
 * at the tick, before any scan.
 *
 * The console's echo is held at receipt and written once, with interrupts
 * on, by a write or by the echo thread, whose passes the rows run
 * (`console_echo_work`), and never by a reader's or poller's drain: a line
 * read by a reader that waits in epoll, poll, select or a blocking read,
 * whose line the tick has already drained, is echoed once. A burst past
 * the tick's budget is echoed whole; a Ctrl-C discards the echo held before
 * it unless NOFLSH; and a hangup discards the echo not yet written. The
 * echo thread and a writer give the echo a batch at a time, with a service
 * point after each and no more than a 16550's FIFO of bytes written with
 * interrupts off, and a writer's hangup or a VSTOP at that service point
 * ends or stops the write.
 */
#define WAIT_QUEUE_HOSTTEST
#define wq_wait_event model_wait_event
#include "proc/wait_queue.c"
#undef wq_wait_event
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg);
#include "tty/tty.c"
#include "tty/pty.c"
#include "ids_first.h"

extern int printf(const char *format, ...);
extern int snprintf(char *str, size_t size, const char *format, ...);
extern int dprintf(int fd, const char *format, ...);
extern int strcmp(const char *a, const char *b);

struct percpu cpu_table[NR_CPUS];
process_t *proc_list;
static process_t consumer, producer, poller;
/* The processes' thread groups, which keep whether each left its terminal. */
static struct tgroup consumer_group, producer_group, poller_group;
static struct pty pair;
static fd_entry_t master, slave;
static int failures, irq_enabled = 1, pending_signal, switches;
static int recording, recorded_waits, read_stamps, serial_pos, serial_len;
static int copy_calls, copy_failure, spans;
/* A row that expects terminal signals sets `signals_expected`; the signals
 * posted are counted, and the last one kept. */
static int signals_expected, signals_posted, last_signo;
/* The console's keyboard is in K_RAW, K_MEDIUMRAW or K_OFF
 * (`kd_translates`). */
static int console_raw;
/* The Scroll Lock flag the console last set (`kd_scroll_lock`), and the
 * serial input `serial_rx_lookahead` has given. */
static int scroll_lock, serial_looked;
static void (*copy_action)(void);
static const char *serial_bytes;
static void (*scheduled_action)(void);
static const char *wake_detail;
static char bytes[512];
volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

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

/** Fill `n` bytes at `ptr` with `c`, as the kernel's string helper does. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *p = ptr;

    while (n--) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

/** Clear the model's interrupt flag and return what it was. */
uint32_t kirq_save(void)
{
    uint32_t saved = (uint32_t)irq_enabled;

    irq_enabled = 0;
    return saved;
}

/* The console's bytes written since interrupts were last on, and the most
 * such bytes seen since a row cleared it: the bytes a 16550 receives
 * meanwhile with no reader of its FIFO. */
static int irqoff_run, irqoff_max;

/** Set the model's interrupt flag to `flags`; turning interrupts on ends
 * the run of bytes written with them off. */
void kirq_restore(uint32_t flags)
{
    irq_enabled = (int)flags;
    if (irq_enabled) {
        irqoff_run = 0;
    }
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

/** Count a terminal signal a row expects; any other is a failure, since no
 * signal substitutes for a readiness wake. */
void signal_post(process_t *p, int signo)
{
    (void)p;
    if (signals_expected) {
        signals_posted++;
        last_signo = signo;
        return;
    }
    CHECK(0, "no signal substitutes for a readiness wake");
}

/** Count a terminal process-group signal through the same stub. */
void signal_post_multi(process_t *p, int signo, const struct k_siginfo *info)
{
    (void)info;
    signal_post(p, signo);
}

/** The terminal rows signal only live leaders. */
process_t *signal_process_target(process_t *p)
{
    return p->state != PROC_UNUSED && p->pid == p->tgid ? p : NULL;
}

/** No row has a PID namespace: the kernel's number, `kid`. */
uint32_t pid_seen(uint32_t kid)
{
    return kid;
}

/** No row has a PID namespace: a terminal keeps no numbers. */
void pid_nums_set(struct pid_nums *n, uint32_t kid)
{
    (void)kid;
    n->ns = NULL;
}

/** No row has a PID namespace: the kernel's number, `kid`. */
uint32_t pid_nums_seen(const struct pid_nums *n, uint32_t kid)
{
    (void)n;
    return kid;
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
 * when this is copy number `copy_failure`. A `copy_action` runs once, as
 * another thread would while the copy sleeps on a page fault. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    void (*action)(void) = copy_action;

    copy_calls++;
    if (copy_calls == copy_failure) {
        return -EFAULT;
    }
    copy_action = NULL;
    if (action) {
        action();
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

/** The model's tick, which the rows set. */
uint64_t rtc_ticks_now(void)
{
    return kernel_ticks;
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

/* What the next service point does, once, as another thread would while
 * the caller gives way there; NULL for nothing. */
static void (*service_action)(void);
/* The service points reached since a row cleared the count. */
static int service_points;

/** Count the service point and run `service_action` once: the rows run one
 * process at a time. */
void sched_service_point(void)
{
    void (*action)(void) = service_action;

    service_points++;
    service_action = NULL;
    if (action) {
        action();
    }
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

/* The bytes the console's sink wrote since a row last cleared
 * `console_bytes`, the last of them, and the first CONSOLE_TEXT_MAX. */
#define CONSOLE_TEXT_MAX 256
static int console_bytes;
static char console_last;
static char console_text[CONSOLE_TEXT_MAX];

/** Count a byte of the console's output, keep it as the last, and keep
 * it in `console_text` while there is room; count it in the run of bytes
 * written with interrupts off while they are off. */
void console_putc_raw(char c)
{
    if (irq_enabled) {
        irqoff_run = 0;
    } else if (++irqoff_run > irqoff_max) {
        irqoff_max = irqoff_run;
    }
    if (console_bytes < CONSOLE_TEXT_MAX) {
        console_text[console_bytes] = c;
    }
    console_bytes++;
    console_last = c;
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

/* The readings of the port, `serial_rx_service`'s calls, whether its
 * receive interrupt is on (`serial_rx_irq_on`) and whether its handler
 * left the line high (`serial_rx_line_high`); a row sets the last two. */
static int serial_services, serial_irq_on, serial_line_high;

/** Count a reading of the port; 0: the test puts whole transfers in the
 * serial input. */
int serial_rx_service(int budget)
{
    (void)budget;
    serial_services++;
    return 0;
}

/** Whether the port's receive interrupt is on, as the row set it; 0 by
 * default, and the tick reads the port each time. */
int serial_rx_irq_on(void)
{
    return serial_irq_on;
}

/** Whether the handler left the line high, as the row set it; 0 by default. */
int serial_rx_line_high(void)
{
    return serial_line_high;
}

/** -1: no serial input has a signal character. */
int serial_rx_find(const unsigned char *set, int n)
{
    (void)set;
    (void)n;
    return -1;
}

/** Give the serial bytes not given before, at most `max`, into `buf`. */
int serial_rx_lookahead(unsigned char *buf, int max)
{
    int n = 0, at = serial_looked > serial_pos ? serial_looked : serial_pos;

    CHECK(!irq_enabled, "the look ahead excludes input interrupts");
    while (at < serial_len && n < max) {
        buf[n++] = (unsigned char)serial_bytes[at++];
    }
    serial_looked = at;
    return n;
}

/** Whether the next serial byte was given by the look ahead. */
int serial_rx_looked(void)
{
    return serial_looked > serial_pos;
}

/** Record the Scroll Lock flag the console sets. */
void kd_scroll_lock(int stopped)
{
    scroll_lock = stopped;
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

/** Whether the console's keyboard translates keys: not while `console_raw`. */
int kd_translates(void)
{
    return !console_raw;
}

/** The screen's blanking, which no row reaches. */
void blank_init(void)
{}

/** The blanking's countdown, which `tty_console_poll` runs first. */
void blank_tick(void)
{}

/** Reset the consumer, current and running, the producer, ready, and the
 * poller, and allocate the pair with both ends open in raw mode without
 * echo or signals. */
static void setup_pair(void)
{
    int i;

    clear(&consumer, sizeof(consumer));
    clear(&producer, sizeof(producer));
    clear(&poller, sizeof(poller));
    clear(&consumer_group, sizeof(consumer_group));
    clear(&producer_group, sizeof(producer_group));
    clear(&poller_group, sizeof(poller_group));
    consumer.tg = &consumer_group;
    producer.tg = &producer_group;
    poller.tg = &poller_group;
    consumer.state = PROC_RUNNING;
    consumer.waiter.proc = &consumer;
    producer.state = PROC_READY;
    producer.pid = 2;
    producer.sid = 1;
    poller.waiter.proc = &poller;
    current_proc = &consumer;
    pending_signal = switches = recording = copy_calls = copy_failure = spans = 0;
    signals_expected = signals_posted = last_signo = 0;
    copy_action = NULL;
    service_action = NULL;
    irq_enabled = 1;
    pty_init_pool();
    CHECK(pty_alloc(0, 0, NULL) == &pair, "production allocation initializes the pair");
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
    tty_set_session(&console_tty, 1, 1, 0);
    recording = 1;
    recorded_waits = read_stamps = serial_pos = serial_len = 0;
    scheduled_action = serial_action;
    wake_detail = "serial notification wakes console reader";
    CHECK(!wq_active(&console_tty.input_wq), "latency check starts without a waiter");
    CHECK(tty_read(&console_tty, out, sizeof(out), 0, console_tty.vhangups) == 2,
          "console read completes");
    CHECK(out[0] == 'q' && out[1] == '\n', "console input is intact");
    CHECK(recorded_waits == 1 && read_stamps == 1, "latency path records wait and read");
    CHECK(switches == 1 && irq_enabled, "console read restores interrupt state");
}

/**
 * With the console's keyboard in a raw mode, the tick's drain and a reader's
 * pump discard COM1's bytes, a Ctrl+C among them, and leave the discipline
 * empty; back in a translating mode, a byte from COM1 is read.
 */
static void raw_serial(void)
{
    unsigned char out[4];

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    console_tty.ldisc.tio.c_lflag &= ~(ICANON | ECHO);
    console_raw = 1;
    serial_bytes = "ab\003";
    serial_pos = 0;
    serial_len = 3;
    irq_enabled = 0;
    tty_console_poll();
    CHECK(serial_pos == serial_len, "a raw console's tick discards COM1's bytes");
    CHECK(n_tty_inq(&console_tty.ldisc) == 0, "no COM1 byte reaches a raw console");
    serial_bytes = "cd";
    serial_pos = 0;
    serial_len = 2;
    console_pump(&console_tty);
    CHECK(serial_pos == serial_len && n_tty_inq(&console_tty.ldisc) == 0,
          "a raw console's reader discards COM1's bytes");
    console_raw = 0;
    serial_bytes = "e";
    serial_pos = 0;
    serial_len = 1;
    tty_console_poll();
    irq_enabled = 1;
    CHECK(tty_read(&console_tty, out, sizeof(out), 0, console_tty.vhangups) == 1 && out[0] == 'e',
          "a translating console reads COM1 again");
}

/**
 * The tick reads COM1 at every call while the port's receive interrupt is
 * off; while it is on, never while the line is low, and every
 * CONSOLE_BACKUP_TICKS, a fifth of a second, while the handler has left it
 * high, the first time at the next tick even after a quiet spell longer
 * than 2^31 ticks, which a 32-bit deadline would wrap into the future.
 */
static void console_poll_backup(void)
{
    int i;

    setup_pair();
    tty_console_init();
    serial_pos = serial_len = 0;
    serial_irq_on = 0;
    serial_services = 0;
    irq_enabled = 0;
    for (i = 0; i < 3; i++) {
        kernel_ticks++;
        tty_console_poll();
    }
    CHECK(serial_services == 3, "the tick reads a port whose interrupt is off at each call");
    serial_irq_on = 1;
    serial_services = 0;
    for (i = 0; i < 2 * (TICK_HZ / 5); i++) {
        kernel_ticks++;
        tty_console_poll();
    }
    CHECK(serial_services == 0, "once the interrupt is on the tick leaves the port to it");
    serial_line_high = 1;
    kernel_ticks++;
    tty_console_poll();
    CHECK(serial_services == 1, "a line the handler left high is read at the next tick");
    for (i = 0; i < TICK_HZ / 5 - 1; i++) {
        kernel_ticks++;
        tty_console_poll();
    }
    CHECK(serial_services == 1, "and not again within a fifth of a second");
    kernel_ticks++;
    tty_console_poll();
    CHECK(serial_services == 2, "and then again, until a reading empties the port");
    serial_line_high = 0;
    kernel_ticks++;
    tty_console_poll();
    kernel_ticks += 0x80000000UL + TICK_HZ;
    tty_console_poll();
    CHECK(serial_services == 2, "a low line is not read through a quiet spell of 2^31 ticks");
    serial_line_high = 1;
    kernel_ticks++;
    tty_console_poll();
    CHECK(serial_services == 3, "and a line left high after it is read at the next tick");
    serial_line_high = 0;
    serial_irq_on = 0;
    irq_enabled = 1;
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

/** Flush the slave's output after the master has blocked in a packet read,
 * as the slave's TCFLSH 1 does. */
static void slave_flush_action(void)
{
    pair.slave.flush(&pair.slave, TTY_FLUSH_OUT);
}

/** A slave hangup discards unread output and leaves the master no packet
 * status, as Linux's __tty_hangup clears pktstatus after its flush. */
static void vhangup_output(void)
{
    wait_queue_t *wq;

    setup_pair();
    CHECK(pts_write(&slave, "old", 3) == 3, "queue output before vhangup");
    tty_vhangup(&pair.slave);
    CHECK(pty_m_avail(&pair) == 0 && !pair.pktstatus,
          "vhangup discards output without packet status outside packet mode");
    setup_pair();
    pair.packet = 1;
    CHECK(pts_write(&slave, "old", 3) == 3, "queue output in packet mode");
    tty_vhangup(&pair.slave);
    CHECK(pty_m_avail(&pair) == 0 && !pair.pktstatus && !ptm_readable(&pair),
          "vhangup in packet mode discards output and leaves no status");
    CHECK(ptm_poll(&master, POLLIN | POLLRDNORM | POLLPRI, &wq) == 0,
          "nothing is readable after vhangup");
}

/** A slave flush in packet mode gives the master TIOCPKT_FLUSHREAD for its
 * input and TIOCPKT_FLUSHWRITE for its output, which it also discards, as
 * Linux's n_tty_packet_mode_flush and pty_flush_buffer do; the status is
 * readable and urgent, and wakes a blocked packet reader. */
static void slave_flush_status(void)
{
    char out[8];
    wait_queue_t *wq;
    uint32_t wakes;

    setup_pair();
    CHECK(pts_write(&slave, "old", 3) == 3, "queue output before a slave flush");
    pair.slave.flush(&pair.slave, TTY_FLUSH_IN | TTY_FLUSH_OUT);
    CHECK(pty_m_avail(&pair) == 0 && !pair.pktstatus,
          "a slave flush discards output without status outside packet mode");
    setup_pair();
    pair.packet = 1;
    pair.slave.flush(&pair.slave, TTY_FLUSH_IN);
    CHECK(ptm_read(&master, out, sizeof(out)) == 1 && out[0] == TIOCPKT_FLUSHREAD,
          "an input flush gives FLUSHREAD alone");
    CHECK(pts_write(&slave, "old", 3) == 3, "queue output in packet mode");
    wakes = wq_wakes(&pair.m_read_wq, WQ_DATA);
    pair.slave.flush(&pair.slave, TTY_FLUSH_IN | TTY_FLUSH_OUT);
    CHECK(pty_m_avail(&pair) == 0 && wq_wakes(&pair.m_read_wq, WQ_DATA) == wakes + 1,
          "an output flush discards output and wakes packet readers");
    CHECK(ptm_poll(&master, POLLIN | POLLRDNORM | POLLPRI, &wq) == (POLLIN | POLLRDNORM | POLLPRI),
          "packet status is readable and urgent");
    CHECK(ptm_read(&master, out, sizeof(out)) == 1 &&
              out[0] == (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE),
          "both flushes give one status byte");
    CHECK(!ptm_readable(&pair), "the read left no output or status");
    scheduled_action = slave_flush_action;
    CHECK(ptm_read(&master, out, sizeof(out)) == 1 && out[0] == TIOCPKT_FLUSHWRITE && switches == 1,
          "an output flush with no output wakes a blocked packet reader");
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
    CHECK(tty_read(&pair.slave, out, sizeof(out), 1, pair.slave.vhangups) == 1 && out[0] == 'a',
          "a read that does not wait takes the input");
    CHECK(spans == 1 && pair.slave.ready_us == 0,
          "a read that does not wait ends the latency span");
}

/** Under OPOST a byte of slave output takes at most two bytes of the
 * master's ring, so the slave is writable, and a newline fits, while two
 * are free, and not while one is. */
static void output_growth(void)
{
    wait_queue_t *wq = NULL;
    char out[4];

    setup_pair();
    current_proc = &producer;
    pair.m_tail = PTY_BUF_SIZE - 1;
    CHECK(pts_poll(&slave, POLLOUT, &wq) == 0, "a ring with one byte free has no room for output");
    pair.m_tail = PTY_BUF_SIZE - 2;
    CHECK(pts_poll(&slave, POLLOUT, &wq) == POLLOUT && pts_write(&slave, "\n", 1) == 1 &&
              switches == 0 && pty_m_avail(&pair) == PTY_BUF_SIZE,
          "the ring takes a newline into its last two bytes");
    pair.m_head = PTY_BUF_SIZE - 2;
    CHECK(ptm_read(&master, out, sizeof(out)) == 2 && out[0] == '\r' && out[1] == '\n',
          "ONLCR makes the newline CR LF");
}

/** Echo of input the master wrote, put in the ring while the slave writer's
 * copy sleeps. */
static void echo_during_copy(void)
{
    pty_slave_sink(&pair, 'e');
    pty_slave_sink(&pair, 'e');
}

/** The master reads the ring empty, which wakes the slave writer; the ring
 * ends with the echo and the first newline. */
static void drain_action(void)
{
    static char out[PTY_BUF_SIZE];
    char *end = out + PTY_BUF_SIZE - 4;

    CHECK(ptm_read(&master, out, sizeof(out)) == PTY_BUF_SIZE && end[0] == 'e' && end[1] == 'e' &&
              end[2] == '\r' && end[3] == '\n',
          "the master reads the echo and the first newline");
}

/** A slave writer that found room for two newlines, and whose copy slept
 * while echo took half of it, writes one, waits for the master to read, and
 * writes the other; no byte is dropped. */
static void copy_refit(void)
{
    char out[4];

    setup_pair();
    pair.m_tail = PTY_BUF_SIZE - 4;
    copy_action = echo_during_copy;
    scheduled_action = drain_action;
    wake_detail = "a master read wakes the slave writer";
    CHECK(pts_write(&slave, "\n\n", 2) == 2 && switches == 1,
          "slave output fits the room left after its copy");
    CHECK(ptm_read(&master, out, sizeof(out)) == 2 && out[0] == '\r' && out[1] == '\n',
          "the second newline follows the master's read");
}

/* The tick at which a timed row's read begins, and the ticks of a tenth of
 * a second, VTIME's unit. */
#define T0 1000u
#define V  MS_TO_TICKS(TTY_VTIME_MS)

/* A step of a timed row, run while the reader waits: the tick it runs at,
 * the bytes that arrive then, or NULL for the timer's wake, and the deadline
 * the reader has armed, 0 for none. */
struct timed_step {
    uint32_t tick;
    const char *input;
    uint32_t deadline;
};

static const struct timed_step *steps;
static int nsteps, step, timed_console;

/** The length of the string `str`. */
static int length(const char *str)
{
    int n = 0;

    while (str[n]) {
        n++;
    }
    return n;
}

/** Run the next step: check the reader's deadline, move the tick, and give
 * the input to the master or the console's serial port, or wake the reader
 * at its deadline as the tick's `proc_wake_sleepers` does. */
static void timed_action(void)
{
    const struct timed_step *s = &steps[step++];

    CHECK(consumer.wakeup_tick == s->deadline, "a timed read arms the deadline POSIX gives it");
    kernel_ticks = s->tick;
    if (s->input && timed_console) {
        serial_bytes = s->input;
        serial_pos = 0;
        serial_len = length(s->input);
        tty_console_poll();
    } else if (s->input) {
        CHECK(ptm_write(&master, s->input, (uint32_t)length(s->input)) == length(s->input),
              "the master writes a timed row's input");
    } else if (consumer.wakeup_tick != 0 && kernel_ticks >= consumer.wakeup_tick) {
        consumer.wakeup_tick = 0;
        consumer.state = PROC_READY;
    }
    if (step < nsteps) {
        scheduled_action = timed_action;
    }
}

/**
 * Read `count` bytes into `out` from the slave, or from the console with
 * `console`, in raw mode with `vmin` and `vtime`, while the `n` steps at `s`
 * run one each time the reader blocks; return what the read returns. The
 * read must block once for each step, run them all and clear its deadline.
 */
static int timed_read(int console, int vmin, int vtime, char *out, int count,
                      const struct timed_step *s, int n)
{
    struct tty *t = console ? &console_tty : &pair.slave;
    int rc;

    setup_pair();
    consumer.sid = 5;
    consumer.tgid = consumer.pid = consumer.pgid = 6;
    if (console) {
        tty_console_init();
        tty_set_session(&console_tty, 5, 6, 0);
        serial_pos = serial_len = 0;
    }
    t->ldisc.tio.c_lflag &= ~(ICANON | ECHO | ISIG);
    t->ldisc.tio.c_cc[VMIN] = (uint8_t)vmin;
    t->ldisc.tio.c_cc[VTIME] = (uint8_t)vtime;
    kernel_ticks = T0;
    steps = s;
    nsteps = n;
    step = 0;
    timed_console = console;
    scheduled_action = n > 0 ? timed_action : NULL;
    wake_detail = "a timed reader wakes for its input or its deadline";
    if (console) {
        rc = tty_read(&console_tty, (unsigned char *)out, count, 0, console_tty.vhangups);
    } else {
        rc = pts_read(&slave, out, (uint32_t)count);
    }
    CHECK(step == n && switches == n, "a timed read blocks once for each step");
    CHECK(consumer.wakeup_tick == 0, "a timed read clears its deadline");
    return rc;
}

/** POSIX's four cases of VMIN and VTIME in a noncanonical read, by ticks. */
static void timed_reads(void)
{
    static const struct timed_step min_only[] = {{T0 + 5, "ab", 0}, {T0 + 500, "c", 0}};
    static const struct timed_step one_byte[] = {{T0 + 5, "a", 0}};
    static const struct timed_step expire[] = {{T0 + 2 * V, NULL, T0 + 2 * V}};
    static const struct timed_step early[] = {{T0 + 5, "a", T0 + 2 * V}};
    static const struct timed_step inter[] = {
        {T0 + 50, "a", 0}, {T0 + 57, "b", T0 + 50 + V}, {T0 + 57 + V, NULL, T0 + 57 + V}};
    static const struct timed_step full[] = {{T0 + 5, "a", 0}, {T0 + 8, "b", T0 + 5 + V}};
    static const struct timed_step serial[] = {{T0 + 3, "k", 0}, {T0 + 3 + V, NULL, T0 + 3 + V}};
    char out[32];

    CHECK(timed_read(0, 0, 0, out, 8, NULL, 0) == 0, "VMIN 0 and VTIME 0 return at once");
    CHECK(timed_read(0, 3, 0, out, 8, min_only, 2) == 3 && out[0] == 'a' && out[2] == 'c',
          "VMIN 3 and VTIME 0 wait for three bytes with no deadline");
    CHECK(timed_read(0, 3, 0, out, 1, one_byte, 1) == 1 && out[0] == 'a',
          "a read for fewer bytes than VMIN returns with as many");
    CHECK(timed_read(0, 0, 2, out, 8, expire, 1) == 0,
          "VMIN 0 and VTIME 2 return none two tenths of a second after the read began");
    CHECK(timed_read(0, 0, 2, out, 8, early, 1) == 1 && out[0] == 'a',
          "VMIN 0 and VTIME 2 return the first byte before the deadline");
    CHECK(timed_read(0, 18, 1, out, 18, inter, 3) == 2 && out[0] == 'a' && out[1] == 'b',
          "VMIN 18 and VTIME 1 wait for a first byte and return short a tenth of a second after "
          "the last");
    CHECK(timed_read(0, 2, 1, out, 8, full, 2) == 2,
          "VMIN 2 and VTIME 1 return once two bytes are there");
    CHECK(timed_read(1, 18, 1, out, 18, serial, 2) == 1 && out[0] == 'k',
          "the console's VMIN 18 and VTIME 1 return a serial byte a tenth of a second after it");
}

/** Close the master while the slave reader waits. */
static void hangup_action(void)
{
    ptm_close(&master);
}

/** A slave reader whose master closes returns 0, and the reads after it
 * return 0 without waiting, though the canonical EOF mark was taken. */
static void hangup_read(void)
{
    char out[4];

    setup_pair();
    consumer.sid = 5;
    consumer.tgid = consumer.pid = consumer.pgid = 6;
    pair.slave.ldisc.tio.c_lflag |= ICANON;
    scheduled_action = hangup_action;
    wake_detail = "the master's close wakes the slave reader";
    CHECK(pts_read(&slave, out, sizeof(out)) == 0 && switches == 1,
          "a slave reader whose master closes reads 0");
    CHECK(pts_read(&slave, out, sizeof(out)) == 0 && switches == 1,
          "a slave read after the master's close reads 0 at once");
}

/** Run every row; 0 when each passes, 1 otherwise. */
static char long_input[N_TTY_LINE_MAX + 1];
static char long_out[N_TTY_READ_MAX];

/** Whether `n` bytes of `long_out` are `long_input`'s line, its 4096th byte
 * dropped: 4095 bytes and the newline. */
static int long_read(int n)
{
    int i;

    if (n != N_TTY_LINE_MAX || long_out[N_TTY_LINE_MAX - 1] != '\n') {
        return 0;
    }
    for (i = 0; i < N_TTY_LINE_MAX - 1; i++) {
        if (long_out[i] != long_input[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * A canonical line holds 4095 bytes before its newline, on a slave and on
 * the console: of 4096 bytes typed and a newline, the 4096th is dropped and
 * counted, and one read returns the line whole though it copies through a
 * bounce of N_TTY_READ_CHUNK. A master writing a second such line behind
 * the first stops short when the slave has no room for a line, drops
 * nothing, and writes the rest once the first is read.
 */
static void long_lines(void)
{
    int i, n, done;

    for (i = 0; i < N_TTY_LINE_MAX; i++) {
        long_input[i] = (char)('a' + i % 26);
    }
    long_input[N_TTY_LINE_MAX] = '\n';

    setup_pair();
    current_proc = &producer;
    pair.slave.ldisc.tio.c_lflag |= ICANON;
    CHECK(ptm_write(&master, long_input, sizeof(long_input)) == (int)sizeof(long_input),
          "the master writes a line of 4096 bytes and a newline");
    CHECK(pair.slave.ldisc.counters.line_drops == 1, "the slave drops the line's 4096th byte");
    n = ptm_write(&master, long_input, sizeof(long_input));
    CHECK(n > 0 && n < (int)sizeof(long_input), "a second line stops short of a full slave");
    CHECK(pts_read(&slave, long_out, sizeof(long_out)) == N_TTY_LINE_MAX &&
              long_read(N_TTY_LINE_MAX),
          "one slave read returns 4095 bytes and the newline");
    CHECK(ptm_write(&master, long_input + n, sizeof(long_input) - n) == (int)sizeof(long_input) - n,
          "the master writes the rest of the second line once the first is read");
    CHECK(pts_read(&slave, long_out, sizeof(long_out)) == N_TTY_LINE_MAX &&
              long_read(N_TTY_LINE_MAX),
          "the second line is read whole");
    CHECK(pair.slave.ldisc.counters.line_drops == 2 && pair.slave.ldisc.counters.queue_drops == 0,
          "a master that waits loses no byte but each line's 4096th");

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    serial_bytes = long_input;
    serial_pos = 0;
    serial_len = (int)sizeof(long_input);
    irq_enabled = 0;
    console_pump(&console_tty);
    irq_enabled = 1;
    CHECK(serial_pos == serial_len && console_tty.ldisc.counters.line_drops == 1,
          "the console takes COM1's line and drops its 4096th byte");
    n = tty_read(&console_tty, (unsigned char *)long_out, N_TTY_READ_CHUNK, 1,
                 console_tty.vhangups);
    for (done = 0; n > 0; n = tty_read_rest(&console_tty, (unsigned char *)long_out + done,
                                            N_TTY_READ_CHUNK, console_tty.vhangups)) {
        done += n;
    }
    CHECK(long_read(done), "the console's read returns 4095 bytes and the newline");
}

/** The packet status a master read gives alone: its byte, or -1 when the
 * read gives data or nothing. */
static int packet_status(void)
{
    char out[8];

    return ptm_read(&master, out, sizeof(out)) == 1 ? (unsigned char)out[0] : -1;
}

/** With a slave writer waiting on stopped output, restart it from the
 * master. */
static void restart_action(void)
{
    CHECK(pair.slave.ldisc.stopped && pty_m_avail(&pair) == 0,
          "the slave writer waits while output is stopped");
    CHECK(ptm_write(&master, "\021", 1) == 1, "the master writes VSTART");
}

/**
 * VSTOP from the master stops the slave's output: it reaches no reader, a
 * master in packet mode reads TIOCPKT_STOP, POLLOUT goes, and a slave writer
 * waits until VSTART wakes it, after which the master reads TIOCPKT_START
 * and then the output.
 */
static void flow_slave(void)
{
    wait_queue_t *wq = NULL;
    char out[8];

    setup_pair();
    pair.packet = 1;
    CHECK(ptm_write(&master, "\023", 1) == 1 && pair.slave.ldisc.stopped,
          "VSTOP from the master stops the slave's output");
    CHECK(n_tty_inq(&pair.slave.ldisc) == 0, "VSTOP reaches no reader");
    CHECK(packet_status() == TIOCPKT_STOP, "a packet master reads TIOCPKT_STOP");
    CHECK(!(pts_poll(&slave, POLLOUT, &wq) & POLLOUT) && wq == &pair.slave.input_wq,
          "stopped output takes POLLOUT from the slave");
    scheduled_action = restart_action;
    wake_detail = "VSTART wakes the slave writer";
    CHECK(pts_write(&slave, "hi", 2) == 2 && switches == 1,
          "the slave writer waits once and writes after VSTART");
    CHECK(packet_status() == TIOCPKT_START, "a packet master reads TIOCPKT_START");
    CHECK(ptm_read(&master, out, sizeof(out)) == 3 && out[0] == 0 && out[1] == 'h',
          "the output follows the status");
}

/** The echo of input typed while output is stopped waits for VSTART. */
static void flow_echo(void)
{
    char out[8];

    setup_pair();
    current_proc = &producer;
    pair.slave.ldisc.tio.c_lflag |= ECHO;
    CHECK(ptm_write(&master, "\023ab", 3) == 3 && pty_m_avail(&pair) == 0,
          "the echo waits while output is stopped");
    CHECK(ptm_write(&master, "\021", 1) == 1 && ptm_read(&master, out, sizeof(out)) == 2 &&
              out[0] == 'a' && out[1] == 'b',
          "VSTART gives out the echo held");
    CHECK(n_tty_inq(&pair.slave.ldisc) == 2, "input typed while stopped is read");
}

/** TCOOFF holds against VSTART until TCOON; TCIOFF and TCION write their
 * characters unless TCOOFF stopped output; another action is EINVAL. */
static void flow_tcflow(void)
{
    char out[8];

    setup_pair();
    current_proc = &producer;
    CHECK(tty_tcflow(&pair.slave, TTY_TCOOFF) == 0 && pair.slave.ldisc.stopped,
          "TCOOFF stops output");
    CHECK(ptm_write(&master, "\021", 1) == 1 && pair.slave.ldisc.stopped,
          "VSTART does not end TCOOFF's stop");
    CHECK(tty_tcflow(&pair.slave, TTY_TCIOFF) == 0 && pty_m_avail(&pair) == 0,
          "TCIOFF writes nothing while TCOOFF holds");
    CHECK(tty_tcflow(&pair.slave, TTY_TCOON) == 0 && !pair.slave.ldisc.stopped,
          "TCOON restarts output");
    CHECK(tty_tcflow(&pair.slave, TTY_TCIOFF) == 0 && tty_tcflow(&pair.slave, TTY_TCION) == 0 &&
              ptm_read(&master, out, sizeof(out)) == 2 && out[0] == 023 && out[1] == 021,
          "TCIOFF and TCION write VSTOP and VSTART");
    CHECK(tty_tcflow(&pair.slave, 4) == -EINVAL, "TCXONC takes four actions");
    CHECK(ptm_tcflow(&pair, TTY_TCOOFF) == 0 && !ptm_can_feed(&pair) &&
              !(ptm_poll(&master, POLLOUT, NULL) & POLLOUT),
          "TCOOFF on the master stops its writes");
    CHECK(ptm_tcflow(&pair, TTY_TCIOFF) == 0 && !pair.slave.ldisc.stopped,
          "the master's TCIOFF writes nothing while its TCOOFF holds");
    CHECK(ptm_tcflow(&pair, TTY_TCOON) == 0 && ptm_tcflow(&pair, TTY_TCIOFF) == 0 &&
              pair.slave.ldisc.stopped,
          "the master's TCIOFF stops the slave's output through its IXON");
}

/** IXON with ^S and ^Q turned off and on gives a packet master
 * TIOCPKT_NOSTOP and TIOCPKT_DOSTOP, and turned off restarts output. */
static void flow_termios(void)
{
    struct ktermios tio;

    setup_pair();
    pair.packet = 1;
    tio = pair.slave.ldisc.tio;
    tio.c_iflag &= ~(unsigned int)IXON;
    tty_set_termios(&pair.slave, &tio, 0);
    CHECK(packet_status() == TIOCPKT_NOSTOP, "IXON off gives TIOCPKT_NOSTOP");
    tio.c_iflag |= IXON;
    tty_set_termios(&pair.slave, &tio, 0);
    CHECK(packet_status() == TIOCPKT_DOSTOP, "IXON on gives TIOCPKT_DOSTOP");
    tio.c_cc[VSTOP] = 023;
    tty_set_termios(&pair.slave, &tio, 0);
    CHECK(!pair.pktstatus, "an unchanged IXON gives no status");
    CHECK(ptm_write(&master, "\023", 1) == 1 && packet_status() == TIOCPKT_STOP,
          "VSTOP stops output");
    tio.c_iflag &= ~(unsigned int)IXON;
    tty_set_termios(&pair.slave, &tio, 0);
    CHECK(!pair.slave.ldisc.stopped && packet_status() == (TIOCPKT_NOSTOP | TIOCPKT_START),
          "IXON off restarts output stopped by VSTOP");
}

/** A signal character discards the slave's output and input and reports both
 * flushes before its echo; under NOFLSH it discards nothing. */
static void signal_flush(void)
{
    char out[8];

    setup_pair();
    current_proc = &producer;
    pair.packet = 1;
    pair.slave.ldisc.tio.c_lflag |= ISIG | ECHO;
    CHECK(pts_write(&slave, "out", 3) == 3 && ptm_write(&master, "in\003", 3) == 3,
          "the slave writes and the master types a line and VINTR");
    CHECK(packet_status() == (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE),
          "VINTR reports both flushes");
    CHECK(ptm_read(&master, out, sizeof(out)) == 3 && out[1] == '^' && out[2] == 'C',
          "VINTR discards the output and the echo before it, then echoes");
    CHECK(n_tty_inq(&pair.slave.ldisc) == 0, "VINTR discards the input");
    pair.slave.ldisc.tio.c_lflag |= NOFLSH;
    CHECK(pts_write(&slave, "out", 3) == 3 && ptm_write(&master, "in\003", 3) == 3,
          "the same under NOFLSH");
    CHECK(ptm_read(&master, out, sizeof(out)) == 8 && out[0] == 0 && out[1] == 'o' &&
              out[4] == 'i' && out[6] == '^',
          "under NOFLSH the output stays and VINTR is echoed in its turn");
    CHECK(n_tty_inq(&pair.slave.ldisc) == 2, "under NOFLSH the input stays");
}

/** A VSTOP the master writes behind a full slave acts at once, and is fed
 * before any wait without reaching the reader or acting after a VSTART. */
static void flow_lookahead(void)
{
    int queued;
    char byte;

    setup_pair();
    fill_input();
    queued = n_tty_inq(&pair.slave.ldisc);
    CHECK(ptm_write(&master, "\023", 1) == 1 && switches == 0 && pair.slave.ldisc.stopped,
          "the master takes the VSTOP and returns before any wait");
    tty_input_char(&pair.slave, 021);
    CHECK(pts_read(&slave, &byte, 1) == 1, "the slave reads a byte");
    CHECK(!pair.slave.ldisc.stopped, "the VSTOP fed after its look ahead does not act again");
    CHECK(n_tty_inq(&pair.slave.ldisc) == queued - 1, "the VSTOP reaches no reader");
}

/** With a console writer waiting on stopped output, VSTART arrives at COM1
 * and the tick services the port. */
static void console_restart_action(void)
{
    CHECK(wq_active(&console_tty.input_wq), "the console writer waits on the queue");
    serial_bytes = "\021";
    serial_pos = serial_looked = 0;
    serial_len = 1;
    tty_console_poll();
}

/**
 * On the console a VSTOP stops output and sets the Scroll Lock flag; a write
 * then writes nothing, POLLOUT goes, a nonblocking wait is EAGAIN, and a
 * blocking one ends when VSTART comes from COM1. A VSTOP behind a full
 * console acts at once. Hold stops and restarts output.
 */
static void flow_console(void)
{
    static char flood[N_TTY_LINE_MAX + 2];
    wait_queue_t *wq = NULL;
    int i;

    setup_pair();
    tty_console_init();
    serial_pos = serial_len = serial_looked = 0;
    tty_input_char(&console_tty, 023);
    CHECK(console_tty.ldisc.stopped && scroll_lock, "VSTOP stops the console and sets Scroll Lock");
    CHECK(tty_write(&console_tty, (const unsigned char *)"x", 1) == 0,
          "a write of stopped output writes nothing");
    irq_enabled = 0;
    CHECK(!(tty_poll(&console_tty, POLLOUT, &wq) & POLLOUT) && wq == &console_tty.input_wq,
          "stopped output takes POLLOUT from the console");
    irq_enabled = 1;
    CHECK(tty_wait_output(&console_tty, 1, &console_tty.vhangups) == -EAGAIN,
          "a nonblocking writer gets EAGAIN");
    scheduled_action = console_restart_action;
    wake_detail = "COM1's VSTART wakes the console writer";
    CHECK(tty_wait_output(&console_tty, 0, &console_tty.vhangups) == 0 && switches == 1,
          "the writer waits once, until VSTART");
    CHECK(!console_tty.ldisc.stopped && !scroll_lock, "VSTART restarts it and clears Scroll Lock");
    CHECK(tty_write(&console_tty, (const unsigned char *)"x", 1) == 1, "the write goes");
    tty_console_hold();
    CHECK(console_tty.ldisc.stopped && scroll_lock, "Hold stops output");
    tty_console_hold();
    CHECK(!console_tty.ldisc.stopped && !scroll_lock, "Hold again restarts it");

    console_tty.ldisc.tio.c_lflag &= ~(ICANON | ECHO);
    for (i = 0; i < N_TTY_LINE_MAX + 1; i++) {
        flood[i] = 'x';
    }
    flood[N_TTY_LINE_MAX + 1] = 023;
    serial_bytes = flood;
    serial_pos = serial_looked = 0;
    serial_len = (int)sizeof(flood);
    irq_enabled = 0;
    console_pump(&console_tty);
    irq_enabled = 1;
    CHECK(console_tty.ldisc.stopped && serial_pos == serial_len - 1,
          "a VSTOP behind a full console acts at once and waits its turn");
    while (n_tty_inq(&console_tty.ldisc) > 0) {
        tty_read(&console_tty, (unsigned char *)bytes, sizeof(bytes), 1, console_tty.vhangups);
    }
    tty_tcflow(&console_tty, TTY_TCOOFF);
    tty_tcflow(&console_tty, TTY_TCOON);
    irq_enabled = 0;
    console_pump(&console_tty);
    irq_enabled = 1;
    CHECK(!console_tty.ldisc.stopped && serial_pos == serial_len &&
              n_tty_inq(&console_tty.ldisc) == 0,
          "the VSTOP fed after its look ahead neither acts again nor is read");
}

/* The slave's output before the echo in `echo_room`, a ring's worth less
 * the two bytes of echo. */
static char big[PTY_BUF_SIZE];

/** With the slave writer waiting behind the echo held, read the ring: the
 * writer's first bytes and the echo fill it. */
static void echo_room_action(void)
{
    static char out[PTY_BUF_SIZE + 1];

    CHECK(ptm_read(&master, out, sizeof(out)) == PTY_BUF_SIZE && out[PTY_BUF_SIZE - 2] == 'a' &&
              out[PTY_BUF_SIZE - 1] == 'b',
          "the slave writer waits with the ring full of its output and the echo held");
}

/**
 * The echo of input typed under TCOOFF waits through TCOON, then goes out
 * ahead of the slave's next write as far as the master's ring has room, and
 * the write waits for the rest of the room instead of losing its bytes
 * behind the echo.
 */
static void echo_room(void)
{
    char out[8];
    int i;

    setup_pair();
    pair.slave.ldisc.tio.c_oflag &= ~(unsigned int)OPOST;
    pair.slave.ldisc.tio.c_lflag |= ECHO;
    for (i = 0; i < (int)sizeof(big); i++) {
        big[i] = 'x';
    }
    CHECK(pts_write(&slave, big, PTY_BUF_SIZE - 2) == PTY_BUF_SIZE - 2,
          "the slave fills the ring but for two bytes");
    CHECK(tty_tcflow(&pair.slave, TTY_TCOOFF) == 0 && ptm_write(&master, "ab", 2) == 2 &&
              pty_m_avail(&pair) == PTY_BUF_SIZE - 2,
          "the echo of input typed under TCOOFF is held");
    CHECK(tty_tcflow(&pair.slave, TTY_TCOON) == 0 && pty_m_avail(&pair) == PTY_BUF_SIZE - 2,
          "TCOON leaves the echo held");
    scheduled_action = echo_room_action;
    wake_detail = "a master read wakes the slave writer";
    CHECK(pts_write(&slave, "YZ", 2) == 2 && switches == 1,
          "the slave writer waits once for room behind the echo");
    CHECK(ptm_read(&master, out, sizeof(out)) == 2 && out[0] == 'Y' && out[1] == 'Z',
          "its bytes follow the echo and none is lost");
}

/** A master write after the last slave description closed feeds the slave,
 * as Linux's does, while the master's read fails with EIO. */
static void master_after_close(void)
{
    char out[4];

    setup_pair();
    current_proc = &producer;
    pts_close(&slave);
    CHECK(pair.hung_up && ptm_write(&master, "x", 1) == 1 && n_tty_inq(&pair.slave.ldisc) == 1,
          "a master write after the last slave close feeds the slave");
    CHECK(ptm_read(&master, out, sizeof(out)) == -EIO, "the master's read still fails with EIO");
}

/* The wakes `hup_rec_wake` has been called for. */
static int hup_wakes;

/** The function of a record that stays on its queue through every wake, a
 * poll(2) poller's or an epoll registration's: count the wake and make the
 * record's thread ready, when it has one. */
static int hup_rec_wake(struct wq_waiter *w, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    (void)nest;
    if (kinds & WQ_FREE) {
        return 0;
    }
    hup_wakes++;
    return w->proc ? wq_ready_deferred(w->proc, cpus) : 0;
}

/* A record on the echo thread's queue, which counts its wakes in
 * `hup_wakes` in place of the thread. */
static struct wq_waiter kick_rec;

/* What a row's first service point saw: the console's bytes, the echo
 * held and the interrupt flag. */
static int seen_bytes, seen_held, seen_irq;

/** Keep what the service point sees (`seen_bytes`). */
static void echo_point_seen(void)
{
    seen_bytes = console_bytes;
    seen_held = (int)(console_tty.ldisc.echo_tail - console_tty.ldisc.echo_head);
    seen_irq = irq_enabled;
}

/** Run the echo thread's passes while its predicate holds, as kconsoled
 * does between its sleeps, at most a full buffer's worth; return the
 * passes run. */
static int echo_thread_drain(void)
{
    int passes;

    for (passes = 0; passes <= N_TTY_ECHO_MAX; passes++) {
        uint32_t flags = kirq_save();
        int due = console_echo_due(NULL);

        kirq_restore(flags);
        if (!due) {
            break;
        }
        console_echo_work();
    }
    return passes;
}

/**
 * A full buffer of keyboard echo held during a stop takes the echo of an
 * ordinary byte COM1's interrupt drains, its oldest byte giving way, as
 * Linux's echo buffer discards its oldest; the byte leaves the discipline
 * without room for a line, and the look ahead acts on the VSTART behind it,
 * whose restart wakes the echo thread, which writes the buffer once, a
 * batch at a time with interrupts on and a service point after each.
 */
static void console_silent_full(void)
{
    struct n_tty *ld = &console_tty.ldisc;
    int k, passes;

    setup_pair();
    tty_console_init();
    serial_pos = serial_len = serial_looked = 0;
    ld->tio.c_lflag &= ~(unsigned int)ICANON;
    tty_input_char(&console_tty, 023);
    for (k = 0; k < N_TTY_ECHO_MAX; k++) {
        tty_input_char(&console_tty, (unsigned char)('a' + k % 26));
    }
    CHECK(ld->stopped && ld->echo_tail - ld->echo_head == N_TTY_ECHO_MAX,
          "a stop holds a full buffer of keyboard echo");
    tty_set_session(&console_tty, 1, 1, 0);
    clear(&kick_rec, sizeof(kick_rec));
    kick_rec.func = hup_rec_wake;
    wq_entry_add(&echo_wq, &kick_rec);
    hup_wakes = 0;
    console_bytes = 0;
    serial_bytes = "q\021";
    serial_len = 2;
    irq_enabled = 0;
    tty_console_poll();
    irq_enabled = 1;
    CHECK(!ld->stopped && serial_pos == 1 && serial_looked == 2 && console_bytes == 0 &&
              ld->echo_tail - ld->echo_head == N_TTY_ECHO_MAX &&
              ld->echo_buf[ld->echo_head & (N_TTY_ECHO_MAX - 1)] == 'b' &&
              ld->echo_buf[(ld->echo_tail - 1) & (N_TTY_ECHO_MAX - 1)] == 'q',
          "COM1's byte holds its echo behind a full buffer, whose oldest byte gives way");
    CHECK(hup_wakes > 0, "COM1's VSTART wakes the echo thread for the echo held");
    service_points = 0;
    seen_bytes = -1;
    service_action = echo_point_seen;
    passes = echo_thread_drain();
    service_action = NULL;
    CHECK(seen_bytes == TTY_ECHO_BATCH && seen_held == N_TTY_ECHO_MAX - TTY_ECHO_BATCH && seen_irq,
          "the echo thread writes a batch of a full buffer and reaches a service point before the "
          "next");
    CHECK(passes == N_TTY_ECHO_MAX / TTY_ECHO_BATCH && service_points == passes,
          "the echo thread reaches a service point after each batch of a full buffer");
    console_echo_work();
    CHECK(console_bytes == N_TTY_ECHO_MAX && console_text[0] == 'b' && console_last == 'q' &&
              ld->echo_tail == ld->echo_head,
          "the echo thread writes the full buffer once");
    wq_entry_remove(&kick_rec);
    tty_set_session(&console_tty, 0, 0, 0);
}

/**
 * Keyboard echo typed while the console's output is stopped stays held when
 * a VSTART from COM1 restarts output in the interrupt's drain, which writes
 * no echo, with no reader waiting, and the next write gives it out first.
 */
static void console_silent_restart(void)
{
    struct n_tty *ld = &console_tty.ldisc;

    setup_pair();
    tty_console_init();
    serial_pos = serial_len = serial_looked = 0;
    ld->tio.c_lflag &= ~(unsigned int)ICANON;
    tty_input_char(&console_tty, 023);
    tty_input_char(&console_tty, 'a');
    tty_input_char(&console_tty, 'b');
    CHECK(ld->stopped && ld->echo_tail - ld->echo_head == 2,
          "keyboard echo typed during a stop is held");
    tty_set_session(&console_tty, 1, 1, 0);
    serial_bytes = "\021";
    serial_len = 1;
    irq_enabled = 0;
    tty_console_poll();
    irq_enabled = 1;
    CHECK(!ld->stopped && serial_pos == 1 && ld->echo_tail - ld->echo_head == 2,
          "COM1's VSTART drained with no reader restarts output and keeps the echo held");
    CHECK(tty_write(&console_tty, (const unsigned char *)"x", 1) == 1 &&
              ld->echo_tail == ld->echo_head,
          "the next write gives the echo out first");
    tty_set_session(&console_tty, 0, 0, 0);
    console_silent_full();
}

/* The open description of a nonblocking master. */
static struct open_file nonblocking_ofd;

/**
 * A nonblocking master write behind a full slave counts the VSTOP it acts
 * on, and the VSTART it acts on as well, which gives out the echo held while
 * output was stopped; neither reaches the reader.
 */
static void lookahead_nonblocking(void)
{
    int queued;

    setup_pair();
    current_proc = &producer;
    fill_input();
    queued = n_tty_inq(&pair.slave.ldisc);
    nonblocking_ofd.status_flags = OFD_NONBLOCK;
    master.ofd = &nonblocking_ofd;
    CHECK(ptm_write(&master, "\023", 1) == 1 && pair.slave.ldisc.stopped &&
              n_tty_inq(&pair.slave.ldisc) == queued,
          "a nonblocking master write behind a full slave counts the VSTOP it acts on");
    pair.slave.ldisc.tio.c_lflag |= ECHO;
    tty_input_char(&pair.slave, 'e');
    CHECK(pty_m_avail(&pair) == 0, "the echo waits while output is stopped");
    CHECK(ptm_write(&master, "\021", 1) == 1 && !pair.slave.ldisc.stopped &&
              pty_m_avail(&pair) == 1 && n_tty_inq(&pair.slave.ldisc) == queued + 1,
          "its VSTART restarts output, gives out the echo held and counts");
    master.ofd = NULL;
}

/**
 * Nonblocking master writes behind a full raw slave, each 255 data bytes and
 * a VSTOP or VSTART, act on and feed no more than the read queue can store:
 * no byte is dropped, and the data bytes each write counts are the bytes the
 * queue gained.
 */
static void lookahead_bounded(void)
{
    char piece[256];
    int queued, n, rc, counted = 0, refused = 0;

    setup_pair();
    current_proc = &producer;
    fill_input();
    queued = n_tty_inq(&pair.slave.ldisc);
    nonblocking_ofd.status_flags = OFD_NONBLOCK;
    master.ofd = &nonblocking_ofd;
    clear(piece, sizeof(piece));
    for (n = 0; n < 255; n++) {
        piece[n] = 'x';
    }
    for (n = 0; n < 20; n++) {
        piece[255] = (char)(n % 2 == 0 ? '\023' : '\021');
        rc = ptm_write(&master, piece, sizeof(piece));
        if (rc == -EAGAIN) {
            refused++;
        } else if (rc > 0) {
            counted += rc > 255 ? 255 : rc;
        }
    }
    CHECK(pair.slave.ldisc.counters.queue_drops == 0,
          "mixed data and flow writes behind a full slave drop no byte");
    CHECK(n_tty_inq(&pair.slave.ldisc) == queued + counted && refused > 0,
          "each counts only the data bytes the queue gained, and a full queue refuses");
    master.ofd = NULL;
}

/* Data accepted from the competing writer in `lookahead_interrupted`. */
static int competing_bytes;

/** Fill the remaining input with nonblocking writes through the shared
 * description, then interrupt the first writer if it slept. */
static void lookahead_compete(void)
{
    char piece[256];
    int n, rc;

    nonblocking_ofd.status_flags = OFD_NONBLOCK;
    for (n = 0; n < 255; n++) {
        piece[n] = 'y';
    }
    for (n = 0; n < 16; n++) {
        piece[255] = pair.slave.ldisc.stopped ? '\021' : '\023';
        rc = ptm_write(&master, piece, sizeof(piece));
        CHECK(rc == 256 || rc == -EAGAIN, "a competing write takes its prefix or refuses it");
        CHECK(!pair.m_writing || rc == -EAGAIN,
              "a nonblocking master cannot enter while another writer sleeps");
        if (rc > 0) {
            competing_bytes += rc - 1;
        }
    }
    pending_signal = 1;
    wq_wake_all(&pair.m_read_wq);
}

/**
 * A blocking writer behind 4097 queued bytes offers 255 data bytes and a
 * VSTOP. If it sleeps, another writer fills all but 15 queue slots through
 * the same description and interrupts it. The accepted prefix must already
 * be stored before that can happen. Without the VSTOP, the first writer
 * really waits, excludes the competing writer and returns EINTR without
 * feeding or counting its data.
 */
static void lookahead_interrupted(void)
{
    char piece[256];
    int flow, n, rc, queued;

    for (n = 0; n < 255; n++) {
        piece[n] = 'x';
    }
    piece[255] = '\023';
    for (flow = 0; flow <= 1; flow++) {
        setup_pair();
        fill_input();
        queued = n_tty_inq(&pair.slave.ldisc);
        CHECK(queued == 4097, "the slave has 4097 bytes before the blocking write");
        nonblocking_ofd.status_flags = 0;
        master.ofd = &nonblocking_ofd;
        competing_bytes = 0;
        scheduled_action = lookahead_compete;
        wake_detail = "the signal wakes the blocked master writer";
        rc = ptm_write(&master, piece, (uint32_t)(255 + flow));
        CHECK(!pair.m_writing, "the master releases its write lock on return or interruption");
        if (!switches) {
            current_proc = &producer;
            lookahead_compete();
        }
        CHECK(rc == (flow ? 256 : -EINTR) && switches == (flow ? 0 : 1),
              "a retained flow prefix returns before sleeping; ordinary data waits for the signal");
        CHECK(pair.slave.ldisc.counters.queue_drops == 0 &&
                  n_tty_inq(&pair.slave.ldisc) == queued + competing_bytes + (flow ? 255 : 0),
              "the interrupted and competing writes count only the data the queue gained");
        master.ofd = NULL;
        scheduled_action = NULL;
    }
}

/** Complete the preceding write and wake the writer waiting for its lock. */
static void master_unlock_action(void)
{
    CHECK(pair.m_writing, "the master writer waits for the preceding write");
    pair.m_writing = 0;
    wq_wake(&pair.m_read_wq, 0);
}

/** A blocking master waits for the preceding write, and a failed copy
 * releases the lock as a successful write does. */
static void master_write_lock(void)
{
    setup_pair();
    pair.m_writing = 1;
    scheduled_action = master_unlock_action;
    wake_detail = "completing a master write wakes the next writer";
    CHECK(ptm_write(&master, "a", 1) == 1 && switches == 1 && !pair.m_writing,
          "the next master write takes the lock and releases it after feeding");
    copy_failure = copy_calls + 1;
    CHECK(ptm_write(&master, "b", 1) == -EFAULT && !pair.m_writing,
          "a failed user copy releases the master write lock");
}

/** Release a preceding writer only after a flow character has waited. */
static void master_flow_unlock(void)
{
    CHECK(n_tty_inq(&pair.slave.ldisc) == 0,
          "a master flow character does not enter while another write holds the lock");
    master_unlock_action();
}

/** TCIOFF and TCION wait for a writer even on a nonblocking master, and
 * an interrupted wait neither feeds a byte nor releases that writer's lock. */
static void master_flow_lock(void)
{
    unsigned long action;
    char out;

    for (action = TTY_TCIOFF; action <= TTY_TCION; action++) {
        setup_pair();
        pair.slave.ldisc.tio.c_iflag &= ~(unsigned int)IXON;
        nonblocking_ofd.status_flags = OFD_NONBLOCK;
        master.ofd = &nonblocking_ofd;
        pair.m_writing = 1;
        scheduled_action = master_flow_unlock;
        wake_detail = "completing a master write wakes a flow character's sender";
        CHECK(ptm_ioctl(&master, TCXONC, action) == 0 && switches == 1 && !pair.m_writing,
              "a nonblocking master's flow ioctl waits for and releases the write lock");
        CHECK(pts_read(&slave, &out, 1) == 1 && out == (action == TTY_TCIOFF ? 023 : 021),
              "the flow character follows the preceding master write");
        pair.m_writing = 1;
        pending_signal = 1;
        CHECK(ptm_ioctl(&master, TCXONC, action) == -EINTR && pair.m_writing &&
                  n_tty_inq(&pair.slave.ldisc) == 0,
              "an interrupted flow ioctl leaves the preceding write and input alone");
        pending_signal = 0;
        pair.m_writing = 0;
        scheduled_action = NULL;
    }
}

/** Try a competing write while the second vector buffer's copy sleeps. */
static void vector_compete(void)
{
    fd_entry_t other = master;

    CHECK(n_tty_inq(&pair.slave.ldisc) == 1,
          "the first vector element was fed before the second copy sleeps");
    other.ofd = &nonblocking_ofd;
    nonblocking_ofd.status_flags = OFD_NONBLOCK;
    CHECK(ptm_write(&other, "x", 1) == -EAGAIN,
          "another master writer cannot enter between vector elements");
}

/** Arrange a fault in the next element after the first copy completes. */
static void vector_first_copy(void)
{
    copy_action = vector_compete;
}

/** A master vector keeps its lock across elements and a failed copy,
 * returning the prefix on a later fault and releasing the lock every time. */
static void master_write_vector(void)
{
    struct kernel_iovec iov[3];
    char out[3];
    int fault;

    iov[0].iov_base = (uintptr_t)"a";
    iov[0].iov_len = 1;
    iov[1].iov_base = 0;
    iov[1].iov_len = 0;
    iov[2].iov_base = (uintptr_t)"b";
    iov[2].iov_len = 1;
    setup_pair();
    copy_action = vector_first_copy;
    CHECK(pty_master_writev(&master, iov, 3) == 2 && copy_calls == 2 && !pair.m_writing,
          "a master writes both vector elements under one lock");
    CHECK(ptm_write(&master, "x", 1) == 1 && pts_read(&slave, out, 3) == 3 && out[0] == 'a' &&
              out[1] == 'b' && out[2] == 'x',
          "the competing byte follows the whole vector");
    for (fault = 1; fault <= 2; fault++) {
        setup_pair();
        copy_failure = fault;
        CHECK(pty_master_writev(&master, iov, 3) == (fault == 1 ? -EFAULT : 1) && !pair.m_writing &&
                  n_tty_inq(&pair.slave.ldisc) == fault - 1,
              "a vector fault returns its completed prefix and releases the master lock");
    }
}

/* The dispositions of the background process in `job_check`. */
static struct k_sigaction job_acts[KSIG_MAX + 1];
/* A thread of the parent's process in `job_check`, alive after its
 * leader, the producer, has exited. */
static process_t parent_thread;

/**
 * A process of a background group of the session the slave controls takes
 * SIGTTOU for a change, goes ahead when it ignores SIGTTOU, fails its read
 * with EIO when it ignores SIGTTIN, and fails with EIO, posting nothing,
 * once its group is orphaned; the foreground group passes. A parent whose
 * leader has exited still counts while another of its threads runs.
 */
static void job_check(void)
{
    setup_pair();
    clear(job_acts, sizeof(job_acts));
    proc_list = &consumer;
    consumer.next = &producer;
    consumer.pid = consumer.tgid = consumer.pgid = 3;
    consumer.sid = 1;
    consumer.parent_pid = 2;
    consumer.space_root = 1;
    consumer.sigacts = job_acts;
    producer.tgid = producer.pgid = 2;
    producer.parent_pid = 1;
    producer.space_root = 1;
    pair.slave.sid = 1;
    pair.slave.fg_pgid = 2;
    signals_expected = 1;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == -EINTR && signals_posted == 1 &&
              last_signo == SIGTTOU,
          "a background process's change takes SIGTTOU");
    job_acts[SIGTTOU].handler = KSIG_IGN;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == 0, "an ignored SIGTTOU lets the change go ahead");
    job_acts[SIGTTIN].handler = KSIG_IGN;
    CHECK(tty_job_check(&pair.slave, SIGTTIN) == -EIO, "an ignored SIGTTIN fails the read");
    job_acts[SIGTTOU].handler = 0;
    producer.sid = 4;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == -EIO && signals_posted == 1,
          "an orphaned group's change is EIO and posts nothing");
    pair.slave.fg_pgid = 3;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == 0, "the foreground group passes");
    pair.slave.fg_pgid = 2;
    producer.sid = 1;
    job_acts[SIGTTOU].handler = KSIG_IGN;
    pair.slave.ldisc.tio.c_lflag |= TOSTOP;
    CHECK(pts_write(&slave, "w", 1) == 1 && signals_posted == 1,
          "a background TOSTOP write that ignores SIGTTOU goes ahead");
    job_acts[SIGTTOU].handler = 0;
    producer.state = PROC_ZOMBIE;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == -EIO && signals_posted == 1,
          "a group whose parent outside it has ended is orphaned");
    clear(&parent_thread, sizeof(parent_thread));
    producer.next = &parent_thread;
    parent_thread.pid = 7;
    parent_thread.tgid = parent_thread.pgid = 2;
    parent_thread.sid = 1;
    parent_thread.parent_pid = 1;
    parent_thread.space_root = 1;
    parent_thread.state = PROC_READY;
    CHECK(tty_job_check(&pair.slave, SIGTTOU) == -EINTR && signals_posted == 2,
          "a parent whose leader exited while a thread runs keeps the group joined");
    producer.next = NULL;
    proc_list = NULL;
}

/** Under TCOOFF the console's TCIOFF writes its VSTOP to the screen and
 * COM1, and the stop and its Scroll Lock stay. */
static void console_tcioff(void)
{
    setup_pair();
    tty_console_init();
    CHECK(tty_tcflow(&console_tty, TTY_TCOOFF) == 0 && scroll_lock, "TCOOFF stops the console");
    console_bytes = 0;
    CHECK(tty_tcflow(&console_tty, TTY_TCIOFF) == 0 && console_bytes == 1 && console_last == 023,
          "the console's TCIOFF writes VSTOP under TCOOFF");
    CHECK(console_tty.ldisc.stopped && scroll_lock, "the stop and its Scroll Lock stay");
    tty_tcflow(&console_tty, TTY_TCOON);
}

/** A session leader's read of a slave no session controls does not make it
 * the leader's controlling terminal, as on Linux. */
static void read_claims_nothing(void)
{
    char byte;

    setup_pair();
    consumer.sid = consumer.tgid = consumer.pgid = 5;
    CHECK(ptm_write(&master, "r", 1) == 1 && pts_read(&slave, &byte, 1) == 1 && byte == 'r',
          "the leader reads the slave");
    CHECK(pair.slave.sid == 0 && pair.slave.fg_pgid == 0, "the read claims no terminal");
}

/** The end of the console's session leader, the next session's claim of
 * the console and its typed line, all while an old reader sleeps. */
static void next_session_action(void)
{
    tty_vhangup_session(&console_tty);
    tty_set_session(&console_tty, 9, 9, 0);
    tty_input_char(&console_tty, 'n');
    tty_input_char(&console_tty, '\n');
}

/**
 * A console reader asleep across the end of its session's leader, which
 * runs only after the next session has claimed the console and typed a
 * line, returns 0 and leaves the line, which the next session's reader
 * reads; a reader with SIGHUP blocked is such a reader, since the
 * foreground group's SIGHUP does not end its wait.
 */
static void hangup_next_session(void)
{
    unsigned char out[4];
    uint32_t opened;

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    serial_pos = serial_len = 0;
    signals_expected = 1;
    opened = console_tty.vhangups;
    scheduled_action = next_session_action;
    wake_detail = "the leader's end wakes the console reader";
    CHECK(tty_read(&console_tty, out, sizeof(out), 0, opened) == 0 && switches == 1,
          "a console reader asleep across its leader's end takes none of the next session's line");
    CHECK(tty_read(&console_tty, out, sizeof(out), 1, opened) == 0,
          "a read that began before the leader's end reads 0 without waiting");
    CHECK(tty_read(&console_tty, out, sizeof(out), 1, console_tty.vhangups) == 2 && out[0] == 'n',
          "the next session's reader reads its line");
    tty_set_session(&console_tty, 0, 0, 0);
}

/** End the console's session leader during a copy from the user. */
static void hangup_copy_action(void)
{
    tty_vhangup_session(&console_tty);
}

/** Arm `hangup_copy_action` for the next copy from the user. */
static void hangup_next_copy(void)
{
    copy_action = hangup_copy_action;
}

/**
 * A writer of a description the end of the console's session leader hangs
 * up while its copy from the user sleeps on a fault writes nothing after
 * the copy: the call returns the bytes written before it, or -EIO when
 * there are none. A write of the console's own description goes on.
 */
static void hangup_writer(void)
{
    uint32_t opened;

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    signals_expected = 1;
    opened = console_tty.vhangups;
    console_bytes = 0;
    copy_action = hangup_next_copy;
    CHECK(tty_write_user(&console_tty, bytes, 300, 0, &opened) == 256 && console_bytes == 256,
          "a console writer whose second copy spans the leader's end returns its first piece");
    tty_set_session(&console_tty, 1, 1, 0);
    opened = console_tty.vhangups;
    console_bytes = 0;
    copy_action = hangup_copy_action;
    CHECK(tty_write_user(&console_tty, bytes, 10, 0, &opened) == -EIO && console_bytes == 0,
          "a console writer whose only copy spans the leader's end writes nothing and gets EIO");
    tty_set_session(&console_tty, 1, 1, 0);
    console_bytes = 0;
    copy_action = hangup_copy_action;
    CHECK(tty_write_user(&console_tty, bytes, 10, 0, NULL) == 10 && console_bytes == 10,
          "a writer of the console's own description writes across the leader's end");
    tty_set_session(&console_tty, 0, 0, 0);
}

/** A console poller that asks for no events waits on the queue the end of
 * the session's leader wakes, and its description's poll then gives
 * POLLHUP, as Linux reports POLLHUP whatever was asked. */
static void hangup_poll_no_events(void)
{
    wait_queue_t *wq = NULL, *hung_wq = NULL;
    uint32_t opened;

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    opened = console_tty.vhangups;
    signals_expected = 1;
    CHECK(tty_console_desc_poll(&opened, 1, 0, &wq) == 0 && wq == &console_tty.input_wq,
          "a console poll for no events gives a queue");
    if (wq) {
        poller.state = PROC_BLOCKED;
        wq_poll_add(wq, &poller.waiter);
        tty_vhangup_session(&console_tty);
        CHECK(poller.state == PROC_READY, "the leader's end wakes a poller for no events");
        wq_poll_remove(wq, &poller.waiter);
    }
    CHECK(tty_console_desc_poll(&opened, 1, 0, &hung_wq) == (short)TTY_HUNG_POLL &&
              (TTY_HUNG_POLL & POLLHUP) != 0,
          "the hung description's poll for no events gives POLLHUP");
    tty_set_session(&console_tty, 0, 0, 0);
}

/** Put `text` in COM1's ring and run the console's tick, with interrupts
 * off as the tick has them. */
static void hup_serial(const char *text)
{
    serial_bytes = text;
    serial_pos = 0;
    serial_len = length(text);
    irq_enabled = 0;
    tty_console_poll();
    irq_enabled = 1;
}

/**
 * Serial input, a Ctrl-C and a Ctrl-Z that come while the console's only
 * waiter waits for the hangup alone reach the discipline and the foreground
 * group at the tick, before any scan, whether the waiter is a poll(2)
 * poller's record, which stays on the queue through the wake, or an epoll
 * registration, which has no thread; the end of the session's leader then
 * wakes the poller's record, and its description reads as hung up, which
 * poll(2) reports as POLLHUP.
 */
static void hangup_poller_drains(void)
{
    struct wq_waiter epoll_rec;
    wait_queue_t *wq = NULL;
    process_t *saved = proc_list;
    uint32_t opened;

    setup_pair();
    tty_console_init();
    opened = console_tty.vhangups;
    proc_list = &producer;
    producer.next = NULL;
    producer.tgid = producer.pgid = 2;
    tty_set_session(&console_tty, 1, 2, 0);
    signals_expected = 1;
    hup_wakes = 0;

    CHECK(tty_console_desc_poll(&opened, 1, 0, &wq) == 0 && wq == &console_tty.input_wq,
          "a console poll for no events gives a queue");
    poller.waiter.func = hup_rec_wake;
    poller.state = PROC_BLOCKED;
    wq_poll_add(&console_tty.input_wq, &poller.waiter);
    hup_serial("ab\n");
    CHECK(serial_pos == serial_len && n_tty_readable(&console_tty.ldisc),
          "the tick drains COM1 into the discipline while a poller for no events waits");
    CHECK(poller.state == PROC_READY && wq_active(&console_tty.input_wq),
          "serial input wakes a poller for no events, which stays on the queue");
    hup_serial("\003");
    CHECK(serial_pos == serial_len && signals_posted == 1 && last_signo == SIGINT,
          "a COM1 Ctrl-C reaches the foreground group before a poller for no events scans");
    hup_serial("\032");
    CHECK(serial_pos == serial_len && signals_posted == 2 && last_signo == SIGTSTP,
          "a COM1 Ctrl-Z reaches the foreground group before a poller for no events scans");
    CHECK(tty_console_desc_poll(&opened, 1, 0, &wq) == 0,
          "the scan of a poller for no events reports none");
    poller.state = PROC_BLOCKED;
    tty_vhangup_session(&console_tty);
    CHECK(poller.state == PROC_READY && tty_desc_hung(&console_tty, opened),
          "the leader's end wakes the poller's record and hangs up its description");
    wq = NULL;
    CHECK(tty_console_desc_poll(&opened, 1, 0, &wq) == (short)TTY_HUNG_POLL &&
              (TTY_HUNG_POLL & POLLHUP) != 0,
          "the hung description's poll for no events gives POLLHUP");
    wq_poll_remove(&console_tty.input_wq, &poller.waiter);
    tty_set_session(&console_tty, 1, 2, 0);
    signals_posted = 0;

    clear(&epoll_rec, sizeof(epoll_rec));
    epoll_rec.func = hup_rec_wake;
    wq_entry_add(&console_tty.input_wq, &epoll_rec);
    hup_wakes = 0;
    hup_serial("cd\n");
    CHECK(serial_pos == serial_len && n_tty_readable(&console_tty.ldisc) && hup_wakes == 1,
          "the tick drains COM1 while only an epoll registration waits, and wakes it");
    hup_serial("\003");
    CHECK(serial_pos == serial_len && signals_posted == 1 && last_signo == SIGINT,
          "a COM1 Ctrl-C reaches the foreground group while only an epoll registration waits");
    wq_entry_remove(&epoll_rec);
    tty_set_session(&console_tty, 0, 0, 0);
    proc_list = saved;
}

/** Whether the console's sink wrote `text` and nothing more since the row
 * last cleared `console_bytes`. */
static int console_said(const char *text)
{
    int i, n = length(text);

    if (console_bytes != n || n > CONSOLE_TEXT_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (console_text[i] != text[i]) {
            return 0;
        }
    }
    return 1;
}

/** Make the console's terminal fresh, canonical with echo, owned by session
 * 1 with process 2's group in the foreground, with COM1's ring empty and
 * no console output counted. The caller restores `proc_list`. */
static void echo_setup(void)
{
    setup_pair();
    tty_console_init();
    proc_list = &producer;
    producer.next = NULL;
    producer.tgid = producer.pgid = 2;
    tty_set_session(&console_tty, 1, 2, 0);
    serial_pos = serial_len = serial_looked = 0;
    console_bytes = 0;
    hup_wakes = 0;
}

/* How the console's reader waits in `echo_once`. */
#define WAIT_EPOLL  0 /* an epoll registration, which has no thread */
#define WAIT_POLL   1 /* a poll(2) poller's record, for POLLIN */
#define WAIT_SELECT 2 /* a select poller's record, for reading and writing */
#define WAIT_READ   3 /* a blocking read */

/* The line `echo_tick_action` types on COM1. */
static const char *echo_line;

/** A line from COM1 at the tick, `echo_line`, the blocked reader's
 * producer: the ring is empty and nothing echoed before the reader runs. */
static void echo_tick_action(void)
{
    serial_bytes = echo_line;
    serial_pos = 0;
    serial_len = 3;
    tty_console_poll();
    CHECK(serial_pos == serial_len && console_bytes == 0,
          "the tick drains a blocked reader's line and writes no echo");
}

/**
 * Type the three bytes of `line` on COM1 while the console's reader waits as
 * `how` says, the tick receiving them with their echo held, and read them
 * into `out`, of `size` bytes; return the read's result.
 */
static int echo_read_line(int how, const char *line, unsigned char *out, int size)
{
    static const char *const names[] = {"an epoll registration", "a poll(2) poller",
                                        "a select poller", "a blocking reader"};
    static char detail[160];
    struct wq_waiter epoll_rec;
    wait_queue_t *wq = NULL;
    short events = how == WAIT_SELECT ? (short)(POLLIN | POLLOUT) : (short)POLLIN;
    int n;

    clear(&epoll_rec, sizeof(epoll_rec));
    epoll_rec.func = hup_rec_wake;
    if (how == WAIT_READ) {
        echo_line = line;
        scheduled_action = echo_tick_action;
        wake_detail = "COM1's line wakes the blocked console reader";
        return tty_read(&console_tty, out, size, 0, console_tty.vhangups);
    }
    if (how == WAIT_EPOLL) {
        wq_entry_add(&console_tty.input_wq, &epoll_rec);
    } else {
        CHECK(tty_poll(&console_tty, events, &wq) == (events & POLLOUT),
              "the console has no input to report before the line");
        poller.waiter.func = hup_rec_wake;
        poller.state = PROC_BLOCKED;
        wq_poll_add(&console_tty.input_wq, &poller.waiter);
    }
    hup_wakes = 0;
    hup_serial(line);
    snprintf(detail, sizeof(detail),
             "the tick drains COM1 and writes no echo while %s waits, and wakes it", names[how]);
    CHECK(serial_pos == serial_len && console_bytes == 0 && hup_wakes > 0, detail);
    CHECK((tty_poll(&console_tty, events, &wq) & POLLIN) != 0, "the rescan finds the line");
    n = tty_read(&console_tty, out, size, 1, console_tty.vhangups);
    if (how == WAIT_EPOLL) {
        wq_entry_remove(&epoll_rec);
    } else {
        wq_poll_remove(&console_tty.input_wq, &poller.waiter);
    }
    return n;
}

/**
 * A line typed on COM1 while the console's reader waits as `how` says is
 * received at the tick, with its echo held, so that the ring is empty when
 * the reader scans or runs; the reader reads the line, the line's echo is
 * written before the read returns, as Linux's n_tty_read flushes echoes,
 * and it reaches the screen exactly once, however often the reader scans
 * and reads and the echo thread runs after it. A session leader that reads
 * a line and ends at once, before the echo thread runs, leaves the line's
 * echo written, not discarded by its hangup.
 */
static void echo_once(int how)
{
    static const char *const names[] = {"an epoll registration", "a poll(2) poller",
                                        "a select poller", "a blocking reader"};
    static char detail[160];
    process_t *saved = proc_list;
    wait_queue_t *wq = NULL;
    short events = how == WAIT_SELECT ? (short)(POLLIN | POLLOUT) : (short)POLLIN;
    unsigned char out[16];
    int n;

    echo_setup();
    n = echo_read_line(how, "ab\n", out, sizeof(out));
    snprintf(detail, sizeof(detail), "%s reads COM1's line", names[how]);
    CHECK(n == 3 && out[0] == 'a' && out[1] == 'b' && out[2] == '\n', detail);
    snprintf(detail, sizeof(detail), "%s writes its line's echo before its read returns",
             names[how]);
    CHECK(console_said("ab\r\n"), detail);
    console_echo_work();
    (void)tty_poll(&console_tty, events, &wq);
    CHECK(tty_read(&console_tty, out, sizeof(out), 1, console_tty.vhangups) == -EAGAIN,
          "nothing more is read");
    (void)tty_inq(&console_tty);
    console_echo_work();
    snprintf(detail, sizeof(detail), "the line read by %s is echoed exactly once", names[how]);
    CHECK(console_said("ab\r\n"), detail);

    console_bytes = 0;
    n = echo_read_line(how, "cd\n", out, sizeof(out));
    signals_expected = 1;
    tty_vhangup_session(&console_tty);
    signals_expected = 0;
    console_echo_work();
    snprintf(detail, sizeof(detail),
             "a leader that reads a line by %s and ends at once leaves its echo written",
             names[how]);
    CHECK(n == 3 && out[0] == 'c' && console_said("cd\r\n"), detail);
    tty_set_session(&console_tty, 0, 0, 0);
    proc_list = saved;
}

/**
 * With no reader, part of a line typed on COM1 and at the keyboard holds
 * its echo at receipt and wakes the echo thread, which writes it once;
 * with ECHO off the input holds no echo and the thread writes nothing.
 */
static void echo_no_reader(void)
{
    process_t *saved = proc_list;
    int due;

    echo_setup();
    clear(&kick_rec, sizeof(kick_rec));
    kick_rec.func = hup_rec_wake;
    wq_entry_add(&echo_wq, &kick_rec);
    hup_serial("ab");
    tty_input_char(&console_tty, 'c');
    irq_enabled = 0;
    due = console_echo_due(NULL);
    irq_enabled = 1;
    CHECK(serial_pos == serial_len && console_bytes == 0 && due && hup_wakes > 0,
          "a partial line from COM1 and the keyboard holds its echo and wakes the echo thread");
    console_echo_work();
    console_echo_work();
    CHECK(console_said("abc"), "the echo thread writes the echo of a partial line once");
    console_tty.ldisc.tio.c_lflag &= ~(unsigned int)ECHO;
    hup_wakes = 0;
    hup_serial("de");
    tty_input_char(&console_tty, 'f');
    console_echo_work();
    CHECK(console_said("abc") && hup_wakes == 0 &&
              console_tty.ldisc.echo_head == console_tty.ldisc.echo_tail,
          "with ECHO off the input holds no echo and the thread writes nothing");
    wq_entry_remove(&kick_rec);
    tty_set_session(&console_tty, 0, 0, 0);
    proc_list = saved;
}

/* The bytes a 16550's receive FIFO holds: those that may arrive while the
 * console writes with interrupts off before COM1's input overruns. */
#define UART_FIFO 16

/* The keyboard echo held in `echo_batches`, past three batches. */
#define ECHO_HELD 200

/** Keep what the service point sees, then end the session's leader. */
static void echo_point_hangup(void)
{
    echo_point_seen();
    tty_vhangup_session(&console_tty);
}

/** Keep what the service point sees, then type a VSTOP. */
static void echo_point_stop(void)
{
    echo_point_seen();
    tty_input_char(&console_tty, 023);
}

/** Make the console fresh (`echo_setup`) and type ECHO_HELD bytes at its
 * keyboard, whose echo is held. */
static void echo_type_held(void)
{
    int i;

    echo_setup();
    for (i = 0; i < ECHO_HELD; i++) {
        tty_input_char(&console_tty, 'k');
    }
}

/**
 * The echo held goes to COM1 with interrupts on, a batch at a time, with a
 * service point after each. After COM1's VSTART restarts output stopped
 * with more than a batch held, a poller's and FIONREAD's drains write none
 * of it, and the echo thread writes it whole with no more than a FIFO's
 * worth written with interrupts off. A writer gives the echo held before
 * its bytes and tests its hangup and output's flow again after each
 * service point: a hangup there ends the write with EIO, its bytes
 * unwritten, and a VSTOP there leaves the rest of the echo held and a
 * nonblocking write with EAGAIN.
 */
static void echo_batches(void)
{
    struct n_tty *ld = &console_tty.ldisc;
    process_t *saved = proc_list;
    wait_queue_t *wq = NULL;
    uint32_t opened;
    int passes;

    echo_type_held();
    hup_serial("\023");
    CHECK(ld->stopped && ld->echo_tail - ld->echo_head == ECHO_HELD && console_bytes == 0,
          "COM1's VSTOP holds the keyboard's echo");
    hup_serial("\021");
    irqoff_max = 0;
    (void)tty_poll(&console_tty, POLLIN, &wq);
    (void)tty_inq(&console_tty);
    CHECK(!ld->stopped && console_bytes == 0,
          "after COM1's VSTART a poller's and FIONREAD's drains write no echo");
    service_points = 0;
    seen_bytes = -1;
    service_action = echo_point_seen;
    passes = echo_thread_drain();
    CHECK(seen_bytes == TTY_ECHO_BATCH && seen_held == ECHO_HELD - TTY_ECHO_BATCH && seen_irq,
          "the echo thread writes a batch with interrupts on and reaches a service point");
    CHECK(console_bytes == ECHO_HELD &&
              passes == (ECHO_HELD + TTY_ECHO_BATCH - 1) / TTY_ECHO_BATCH &&
              service_points == passes && irqoff_max <= UART_FIFO,
          "the echo thread writes the echo held whole, at most a FIFO's worth with interrupts off");

    echo_type_held();
    signals_expected = 1;
    opened = console_tty.vhangups;
    copy_failure = 0;
    seen_bytes = -1;
    service_action = echo_point_hangup;
    CHECK(tty_write_user(&console_tty, (const char *)"z", 1, 0, &opened) == -EIO &&
              seen_bytes == TTY_ECHO_BATCH && console_bytes == TTY_ECHO_BATCH &&
              console_last == 'k',
          "a writer's hangup at the service point after a batch of the echo ends it with EIO");

    echo_type_held();
    opened = console_tty.vhangups;
    seen_bytes = -1;
    service_action = echo_point_stop;
    CHECK(tty_write_user(&console_tty, (const char *)"z", 1, 1, &opened) == -EAGAIN &&
              seen_bytes == TTY_ECHO_BATCH && console_bytes == TTY_ECHO_BATCH && ld->stopped &&
              ld->echo_tail - ld->echo_head == ECHO_HELD - TTY_ECHO_BATCH,
          "a VSTOP at a writer's service point keeps the rest of the echo held and its bytes");
    service_action = NULL;
    tty_set_session(&console_tty, 0, 0, 0);
    proc_list = saved;
}

/* A burst from COM1 longer than the tick's budget: 99 bytes and a newline. */
static char burst[101];

/**
 * The echo held at the console's receipt: a burst longer than the tick's
 * budget is echoed whole, across the tick, a reader's drain and the echo
 * thread; a Ctrl-C discards the echo held before it, unless NOFLSH, and its
 * own echo follows; and the end of the session's leader discards the echo
 * not yet written.
 */
static void echo_lifecycle(void)
{
    process_t *saved = proc_list;
    unsigned char out[128];
    int i;

    echo_setup();
    for (i = 0; i < 99; i++) {
        burst[i] = 'x';
    }
    burst[99] = '\n';
    burst[100] = 0;
    hup_serial(burst);
    CHECK(serial_pos == TTY_PUMP_BUDGET && console_bytes == 0,
          "the tick drains its budget of a burst and writes no echo");
    CHECK(tty_read(&console_tty, out, sizeof(out), 1, console_tty.vhangups) == 100,
          "a reader drains the rest of the burst and reads its line");
    console_echo_work();
    console_echo_work();
    CHECK(console_bytes == 101 && console_text[0] == 'x' && console_text[98] == 'x' &&
              console_text[99] == '\r' && console_text[100] == '\n',
          "a burst longer than the tick's budget is echoed whole, once");

    echo_setup();
    signals_expected = 1;
    tty_input_char(&console_tty, 'a');
    tty_input_char(&console_tty, 'b');
    CHECK(console_bytes == 0, "keyboard input writes no echo at receipt");
    tty_input_char(&console_tty, 003);
    console_echo_work();
    CHECK(signals_posted == 1 && last_signo == SIGINT && console_said("^C"),
          "a Ctrl-C discards the echo held before it, and its own echo follows");
    console_tty.ldisc.tio.c_lflag |= NOFLSH;
    console_bytes = 0;
    tty_input_char(&console_tty, 'a');
    tty_input_char(&console_tty, 'b');
    tty_input_char(&console_tty, 003);
    console_echo_work();
    CHECK(signals_posted == 2 && console_said("ab^C"),
          "under NOFLSH a Ctrl-C keeps the echo held before its own");

    echo_setup();
    signals_expected = 1;
    tty_input_char(&console_tty, 'a');
    hup_serial("b");
    tty_vhangup_session(&console_tty);
    console_echo_work();
    CHECK(console_bytes == 0 && console_tty.ldisc.echo_head == console_tty.ldisc.echo_tail,
          "the leader's end discards the echo not yet written");
    tty_set_session(&console_tty, 0, 0, 0);
    proc_list = saved;
}

/**
 * A console reader whose line outlasts its buffer, as the read's copy to
 * the user sees it, takes none of the next session's line in the rest it
 * reads after the end of its session's leader.
 */
static void hangup_read_rest(void)
{
    unsigned char out[4];
    uint32_t opened;
    const char *line = "abcdef\n";
    int i;

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    serial_pos = serial_len = 0;
    signals_expected = 1;
    opened = console_tty.vhangups;
    for (i = 0; line[i]; i++) {
        tty_input_char(&console_tty, (unsigned char)line[i]);
    }
    CHECK(tty_read(&console_tty, out, 2, 1, opened) == 2 && out[0] == 'a' &&
              n_tty_line_open(&console_tty.ldisc),
          "a console read takes the start of a line longer than its buffer");
    next_session_action();
    CHECK(tty_read_rest(&console_tty, out, sizeof(out), opened) == 0,
          "the rest a reader reads after its leader's end is none of the next session's line");
    CHECK(tty_read(&console_tty, out, sizeof(out), 1, console_tty.vhangups) == 2 && out[0] == 'n',
          "the next session's reader reads its line");
    tty_set_session(&console_tty, 0, 0, 0);
}

/** Stop the console's output by a ^S from the keyboard. */
static void stop_console_action(void)
{
    tty_input_char(&console_tty, 023);
}

/** Arm `stop_console_action` for the next copy from the user. */
static void stop_next_copy(void)
{
    copy_action = stop_console_action;
}

/** End the console's session leader, then restart its output by a ^Q. */
static void hangup_restart_action(void)
{
    tty_vhangup_session(&console_tty);
    tty_input_char(&console_tty, 021);
}

/**
 * A console writer of a description the end of the session's leader hangs
 * up, at the service point between two pieces or while the writer waits
 * for stopped output, writes nothing more and returns the bytes written
 * before, or -EIO when there are none. A writer of the console's own
 * description waits for output to restart and writes the rest.
 */
static void hangup_writer_waits(void)
{
    uint32_t opened;

    setup_pair();
    tty_console_init();
    tty_set_session(&console_tty, 1, 1, 0);
    signals_expected = 1;
    opened = console_tty.vhangups;
    console_bytes = 0;
    service_action = hangup_copy_action;
    CHECK(tty_write_user(&console_tty, bytes, 300, 0, &opened) == 256 && console_bytes == 256,
          "a console writer whose leader ends at the service point returns its first piece");

    tty_set_session(&console_tty, 1, 1, 0);
    opened = console_tty.vhangups;
    console_bytes = 0;
    copy_action = stop_next_copy;
    scheduled_action = hangup_copy_action;
    wake_detail = "the leader's end wakes a writer that waits for stopped output";
    CHECK(tty_write_user(&console_tty, bytes, 300, 0, &opened) == 256 && console_bytes == 256,
          "a console writer that waits for stopped output across its leader's end returns its "
          "first piece");
    tty_input_char(&console_tty, 021);

    tty_set_session(&console_tty, 1, 1, 0);
    opened = console_tty.vhangups;
    console_bytes = 0;
    stop_console_action();
    scheduled_action = hangup_copy_action;
    CHECK(tty_write_user(&console_tty, bytes, 10, 0, &opened) == -EIO && console_bytes == 0,
          "a console writer that waits for stopped output from its start gets EIO at its "
          "leader's end");
    tty_input_char(&console_tty, 021);

    tty_set_session(&console_tty, 1, 1, 0);
    console_bytes = 0;
    stop_console_action();
    scheduled_action = hangup_restart_action;
    wake_detail = "the restart wakes a writer of the console's own description";
    CHECK(tty_write_user(&console_tty, bytes, 10, 0, NULL) == 10 && console_bytes == 10,
          "a writer of the console's own description waits across the leader's end and writes");
    tty_set_session(&console_tty, 0, 0, 0);
}

/* The devpts mount at /dev/pts the rows give `vfs_devpts_options`, of
 * instance 1. */
static int devpts_mounted;
static struct vfs_pts_options devpts_opts;

/** The options of the devpts mount the rows put at /dev/pts, if any. */
int vfs_devpts_options(struct vfs_pts_options *out, uint32_t *inst)
{
    *inst = devpts_mounted ? 1u : 0u;
    if (devpts_mounted) {
        *out = devpts_opts;
    }
    return devpts_mounted;
}

/** Whether `options` parse, as Linux's devpts parse_mount_options does. */
static int pts_parses(const char *options)
{
    struct vfs_pts_options o;

    return pty_parse_options(options, &o) == 0;
}

/** devpts's options as Linux's parse_mount_options reads them. */
static void devpts_parse_rows(void)
{
    struct vfs_pts_options o;

    CHECK(pty_parse_options(NULL, &o) == 0 && !o.setuid && !o.setgid && o.mode == 0600 &&
              o.ptmxmode == 0 && o.max == PTY_MAX_PAIRS,
          "devpts's defaults are Linux's: no owner, mode 0600, ptmxmode 0000, max 1048576");
    CHECK(pty_parse_options("uid=0x10,gid=5,mode=620,ptmxmode=0666,newinstance,max=16,,", &o) ==
                  0 &&
              o.setuid && o.uid == 16 && o.setgid && o.gid == 5 && o.mode == 0620 &&
              o.ptmxmode == 0666 && o.max == 16,
          "every devpts option is taken");
    CHECK(!pts_parses("uid=-1") && !pts_parses("uid=2147483648") && !pts_parses("uid=5x") &&
              !pts_parses("uid") && !pts_parses("mode=8") && !pts_parses("mode=0x1") &&
              !pts_parses("max=-1") && !pts_parses("max=1048577") && !pts_parses("newinstance=1") &&
              !pts_parses("bogus") && pts_parses("max=1048576"),
          "a bad number, a missing value or another option is EINVAL");
}

/**
 * Two devpts instances, each numbering its pairs from 0 as Linux's
 * devpts_new_index does in its own superblock: a slave is found by its
 * instance and index, listed in its instance alone, and a new pair takes
 * the lowest index free in its instance, which `max=` counts.
 */
static void devpts_instances(void)
{
    static struct pty first, second;
    struct vfs_pts_options o;
    struct pty *p;

    pty_init_pool();
    first.index = second.index = 0;
    first.inst = 0;
    second.inst = 1;
    first.master_open = second.master_open = 1;
    first.next = &second;
    second.next = NULL;
    ptys = &first;
    CHECK(pty_get(0, 0) == &first && pty_get(1, 0) == &second && pty_get(2, 0) == NULL,
          "each instance has its own /dev/pts/0");
    CHECK(pty_index_limit(1) == 1 && pty_is_allocated(1, 0) && !pty_is_allocated(1, 1),
          "an instance lists its own pairs");
    p = pty_alloc(1, 0, NULL);
    CHECK(p && p->inst == 1 && p->index == 1, "a new pair takes the lowest index of its instance");
    ptys = &first; /* the test's heap pair goes back to the heap's */
    first.next = &second;
    pty_parse_options("max=1", &o);
    CHECK(pty_open_master_in(1, &o, 0) == -ENOSPC, "max= counts the pairs of its own instance");
    pty_init_pool();
}

/**
 * devpts's options, a new slave's owner, group and mode from them as
 * devpts_pty_new gives them, and ENOSPC from /dev/ptmx past `max`.
 */
static void devpts_options(void)
{
    struct vfs_pts_options o;
    struct pty *p;

    devpts_parse_rows();
    pty_init_pool();
    consumer.euid = 1000;
    consumer.egid = 100;
    current_proc = &consumer;
    pty_parse_options("mode=0640", &o);
    p = pty_alloc(0, 0, &o);
    CHECK(p && p->uid == 1000 && p->gid == 100 && p->mode == 0640,
          "without uid= and gid= a slave is the opener's, with the mount's mode");
    pty_init_pool(); /* the test's heap has one pair */
    pty_parse_options("uid=7,gid=5", &o);
    p = pty_alloc(0, 0, &o);
    CHECK(p && p->uid == 7 && p->gid == 5 && p->mode == 0600,
          "with them it is theirs, mode 0600 by default");
    pty_init_pool();
    p = pty_alloc(0, 0, NULL);
    CHECK(p && p->uid == 1000 && p->gid == PTY_GID && p->mode == 0620,
          "the kernel's own /dev/pts gives the opener, group tty and mode 0620");
    pty_parse_options("max=1", &devpts_opts);
    devpts_mounted = 1;
    p->inst = 1; /* the one pair is the mounted instance's */
    CHECK(pty_open_master(0) == -ENOSPC, "/dev/ptmx past the mount's max is ENOSPC");
    devpts_mounted = 0;
    devpts_instances();
    consumer.euid = consumer.egid = 0;
    pty_init_pool();
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--devpts-only") == 0) {
        devpts_options();
        printf("tty_hosttest: %s\n", failures == 0 ? "OK" : "failed");
        return failures != 0;
    }
    read_wakes_poll();
    flush_wakes_writer(0);
    flush_wakes_writer(1);
    partial_wakes_reader(0);
    partial_wakes_reader(1);
    latency_read();
    raw_serial();
    console_poll_backup();
    wake_kinds();
    input_room();
    packet_mode();
    vhangup_output();
    slave_flush_status();
    nonblocking_latency();
    output_growth();
    copy_refit();
    timed_reads();
    hangup_read();
    long_lines();
    flow_slave();
    flow_echo();
    flow_tcflow();
    flow_termios();
    signal_flush();
    flow_lookahead();
    flow_console();
    echo_room();
    master_after_close();
    console_silent_restart();
    devpts_options();
    lookahead_nonblocking();
    lookahead_bounded();
    lookahead_interrupted();
    master_write_lock();
    master_write_vector();
    master_flow_lock();
    job_check();
    console_tcioff();
    read_claims_nothing();
    hangup_next_session();
    hangup_writer();
    hangup_poll_no_events();
    hangup_poller_drains();
    echo_once(WAIT_EPOLL);
    echo_once(WAIT_POLL);
    echo_once(WAIT_SELECT);
    echo_once(WAIT_READ);
    echo_no_reader();
    echo_lifecycle();
    echo_batches();
    hangup_read_rest();
    hangup_writer_waits();
    printf("tty-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}

/** Nothing: no row checks the stamps of a USB reader. */
void usb_stamp_wait(uint32_t pid)
{
    (void)pid;
}
