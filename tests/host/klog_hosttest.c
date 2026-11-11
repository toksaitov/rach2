/**
 * klog_hosttest.c: the kernel log (`klog.c`), its records, its ring and its
 * four destinations.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/tick.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * klog.c is included rather than linked, so each case can put the ring and the
 * recorders back to their initial state; the kernel has one boot per run and
 * needs no such thing. The four destinations are replaced by recorders, which
 * is what lets the cases here be the ones a booting machine will not produce
 * on demand: a ring that wraps mid-record, a serial port that never becomes
 * ready, a record longer than the buffer, and a record raised from inside the
 * delivery of another, which is what an interrupt handler that prints does.
 *
 * The records are raised through `klog`, with the arguments a kernel call
 * would pass.
 *
 * Two things are checked about interrupts, and one of them is checked
 * indirectly. That a re-entrant record does not tear the one being delivered,
 * and is printed after it rather than lost, is checked outright by re-entering
 * from the screen recorder. That the formatter's shared sink is protected is
 * checked by counting the brackets: one klog call takes exactly three, the
 * build, the ring append and the look at the queue, and they balance. A host
 * has no interrupts to land in the middle, so the count is what stands in for
 * the case that cannot be staged here.
 *
 * `host_cpu` is the processor a call runs on, and processor 0 holds the big
 * lock (`smp_serialized`), so a row makes a record on processor 1 to have it
 * kept for later, as a record made in an NMI or without the big lock is.
 */

/* The kernel headers define the fixed-width types themselves, so nothing
 * from the host's stdint may be included alongside them. */
extern int printf(const char *fmt, ...);

#include <setjmp.h>

#include "../../rach2/sys/klog.h"

static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* ---- what the destinations do here ---- */

#define CAP 65536

static char screen[CAP];
static unsigned screen_len;
static char serial[CAP];
static unsigned serial_len;
static char net[CAP];
static unsigned net_len;
static char order[64];
static unsigned order_len;

static int serial_wedged;        /* the port never becomes ready */
static int net_on;               /* netconsole has a device to send through */
static int reenter_from_screen;  /* raise a record from inside delivery */
static int reenter_times;        /* how many to raise at the next byte */
static int reenter_after;        /* raise one at this many bytes from now */
static unsigned serial_offered;  /* bytes handed to a port that took none */
static unsigned ring_at_screen;  /* the ring's size when the screen was written */
static int irq_depth;            /* the brackets of the interrupts now open */
static int irq_pairs;            /* the brackets closed */
static int irq_unbalanced;       /* the closes with none open */
static int kept_then_nested;     /* at the screen: keep a record, then make one */
static int holder_at_serial;     /* at the port: processor 0 makes a record */
static int kept_at_serial;       /* at the port: processor 2 keeps a record */
static int panicking;            /* what `k_panicking` says */
static int relax_frees;          /* the other writer finishes at a wait's turn */
static unsigned relax_turns;     /* the turns of a wait for the other writer */
static unsigned serial_at_relax; /* the port's bytes when the other finished */
static int stop_at_serial;       /* the panic stops this processor at this byte */
static int stop_at_net;          /* the same, at this byte handed to netconsole */
static jmp_buf stopped;          /* where a processor the panic stopped goes */

unsigned int host_cpu;

/* The calls of each destination. A destination is noted in `order` at its
 * first call, and not per byte, so a destination that accepted nothing (a
 * wedged port) is still recorded as having been offered the record. */
static unsigned screen_calls, serial_calls, net_calls;

/** Note destination `c` in `order`. */
static void note(char c)
{
    if (order_len < sizeof(order) - 1) {
        order[order_len++] = c;
    }
}

/**
 * The screen's recorder: keep `c`, note the ring's size at the first byte, and
 * raise the records a row asks for from inside the delivery.
 */
void k_putc(char c)
{
    if (screen_calls++ == 0) {
        note('S');
        /* The ring's size now shows whether the ring came first. */
        ring_at_screen = klog_len();
    }
    if (screen_len < CAP) {
        screen[screen_len++] = c;
    }
    if (reenter_from_screen) {
        reenter_from_screen = 0;
        klog(KLOG_ERR, "irq", "landed: in the middle of another record");
    }
    while (reenter_times > 0) {
        reenter_times--;
        klog(KLOG_ERR, "irq", "landed: one of a burst");
    }
    if (reenter_after > 0 && --reenter_after == 0) {
        klog(KLOG_INFO, "usbdisk", "ready: device 4: lun 0");
    }
    if (kept_then_nested) {
        kept_then_nested = 0;
        host_cpu = 1;
        klog(KLOG_ERR, "nmi", "kept: made in an NMI");
        host_cpu = 0;
        klog(KLOG_ERR, "irq", "landed: after the kept one");
    }
}

/**
 * The serial port's recorder: count `c` as offered and keep it unless the port
 * is wedged.
 */
void serial_out(char c)
{
    if (serial_calls++ == 0) {
        note('U');
    }
    serial_offered++;
    if (holder_at_serial) {
        unsigned int was = host_cpu;

        holder_at_serial = 0;
        host_cpu = 0;
        klog(KLOG_ERR, "holder", "back: while another writes the kept records");
        host_cpu = was;
    }
    if (kept_at_serial) {
        unsigned int was = host_cpu;

        kept_at_serial = 0;
        host_cpu = 2;
        klog(KLOG_ERR, "nmi", "kept: while another writes the kept records");
        host_cpu = was;
    }
    if (serial_wedged) {
        return; /* the transmit wait gave up */
    }
    if (serial_len < CAP) {
        serial[serial_len++] = c;
    }
    if (stop_at_serial > 0 && --stop_at_serial == 0) {
        longjmp(stopped, 1);
    }
}

/** Whether netconsole has a device: `net_on`. */
int netconsole_is_enabled(void)
{
    return net_on;
}

/** netconsole's recorder. netconsole holds what it is given until it has a
 * device, and sends it then. `net_on` stands for having one: what is
 * recorded is what netconsole was handed, and what would reach the wire is
 * what it was handed while `net_on`. */
void netconsole_putc(char c)
{
    if (net_calls++ == 0) {
        note('N');
    }
    if (net_on && net_len < CAP) {
        net[net_len++] = c;
    }
    if (stop_at_net > 0 && --stop_at_net == 0) {
        longjmp(stopped, 1);
    }
}

/** No ramoops region here: the console zone takes nothing. */
void pstore_console_write(const char *text, uint32_t n)
{
    (void)text;
    (void)n;
}

/* The stubs the destinations resolve to. The colour setter takes the screen
 * driver's own enumeration, so its header comes first and the definition
 * follows the declaration it is standing in for. */
#include "../../rach2/sys/display/vga_text.h"

/** The screen's colour is not recorded. */
void set_fore_color(display_color_t color)
{
    (void)color;
}

/* The cursor holds the screen's writes take, counted: a record delivered
 * to the screen and a program's console write each hold once and release
 * once, and the count is what says the writers still bracket their
 * bytes. */
static int holds, releases;

/** Count a hold of the cursor. */
void textcon_hold(void)
{
    holds++;
}

/** Count a release of the cursor. */
void textcon_release(void)
{
    releases++;
}

/** Open a bracket of the interrupts: 0. */
unsigned int kirq_save(void)
{
    irq_depth++;
    return 0;
}

/** Close a bracket of the interrupts, counting a close with none open. */
void kirq_restore(unsigned int flags)
{
    (void)flags;
    if (irq_depth == 0) {
        irq_unbalanced++;
    } else {
        irq_depth--;
        irq_pairs++;
    }
}

volatile uint64_t kernel_ticks;

/** Processor 0 holds the big lock: its records are written at once, and
 * another's are kept for later. */
int smp_serialized(void)
{
    return host_cpu == 0;
}

/** Whether this processor has begun a panic: `panicking`. */
int k_panicking(void)
{
    return panicking;
}

/** A turn of a wait: the ring's lock is never held by another here, and
 * while `relax_frees` another processor writing the kept records out
 * finishes at the wait's first turn, which notes the port's bytes then. */
static void relax_turn(void);

void arch_relax(void)
{
    relax_turns++;
    relax_turn();
}

/** No lockup detector to touch here. */
void touch_nmi_watchdog(void)
{}

/** One processor: the exchange cannot lose. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** One processor: the exchange cannot lose. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

/** The console's lock is always free here, and is not counted among the
 * interrupt brackets. */
int console_lock_bounded(uint32_t *flags, uint32_t us)
{
    (void)us;
    *flags = 0;
    return 1;
}

/** Give back the console's lock, which is not modelled. */
void console_unlock(uint32_t flags)
{
    (void)flags;
}

#include "../../rach2/sys/klog.c"

/** The other writer's end at a wait's turn, while `relax_frees`. */
static void relax_turn(void)
{
    if (relax_frees && defer_taking != 0u) {
        serial_at_relax = serial_len;
        defer_taking = 0;
    }
}

/* ---- helpers ---- */

/**
 * Put the ring, the queue, the recorders and the counts back to their initial
 * state.
 */
static void reset(void)
{
    unsigned i;

    screen_len = serial_len = net_len = order_len = 0;
    screen_calls = serial_calls = net_calls = 0;
    holds = releases = 0;
    serial_offered = 0;
    ring_at_screen = 0;
    klog_delivering = 0;
    console_writing = 0;
    serial_wedged = 0;
    net_on = 0;
    reenter_from_screen = 0;
    reenter_times = 0;
    reenter_after = 0;
    pending_first = pending_next = 0;
    pending_lost = 0;
    pending_writing = 0;
    pending_had = 0;
    irq_depth = irq_pairs = irq_unbalanced = 0;
    kernel_ticks = 0;
    ring_st[0].head = ring_st[0].used = ring_st[0].oldest = ring_st[0].written = 0;
    ring_st[0].kept = 0;
    ring_st[1] = ring_st[0];
    ring_cur = 0;
    for (i = 0; i < NR_CPUS; i++) {
        console_held[i] = 0;
    }
    for (i = 0; i < KLOG_RING_SIZE; i++) {
        ring[i] = 0;
    }
    host_cpu = 0;
    kept_then_nested = holder_at_serial = kept_at_serial = panicking = 0;
    stop_at_serial = stop_at_net = 0;
    relax_frees = 0;
    relax_turns = serial_at_relax = 0;
    defer_head = defer_tail = defer_lost = 0;
    defer_skip = 0;
    defer_taking = klog_users = 0;
    ring_lock = 0;
    for (i = 0; i < KLOG_DEFER_MAX; i++) {
        defer_whole[i] = 0;
    }
    screen[0] = serial[0] = net[0] = order[0] = '\0';
}

/**
 * Read the whole ring into `out`, as a string of at most `cap` - 1 bytes; its
 * length.
 */
static unsigned ring_text(char *out, unsigned cap)
{
    unsigned n = klog_read(out, cap - 1, 0);

    out[n] = '\0';
    return n;
}

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `needle` occurs in `hay`. */
static int has(const char *hay, const char *needle)
{
    const char *p;

    for (p = hay; *p != '\0'; p++) {
        const char *a = p, *b = needle;

        while (*b != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

/** Write into `out` the filler record `n` of the eviction row as the logger
 * renders it, so that the ring's contents can be predicted rather than
 * described; its length. `cap` is not read: a record fits 128 bytes. */
static unsigned render_fill(char *out, unsigned cap, int n)
{
    const char *head = "<7>[0.000000] fill: record: ";
    const char *tail = " of two thousand\n";
    char digits[12];
    unsigned len = 0, i;
    int d = 0;

    (void)cap;
    while (head[len] != '\0') {
        out[len] = head[len];
        len++;
    }
    if (n == 0) {
        digits[d++] = '0';
    }
    while (n > 0) {
        digits[d++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (d > 0) {
        out[len++] = digits[--d];
    }
    for (i = 0; tail[i] != '\0'; i++) {
        out[len++] = tail[i];
    }
    return len;
}

/** The newlines in `s`. */
static unsigned count_lines(const char *s)
{
    unsigned n = 0;

    while (*s != '\0') {
        if (*s++ == '\n') {
            n++;
        }
    }
    return n;
}

/**
 * The console writes: a record raised in the middle of one waits for its
 * end and follows it whole, for a program's write and for both of the
 * debug shell's calls.
 *
 * The bytes a program wrote in one call reach the port together and the
 * record follows them. Without the bracket the record would end the open
 * line and split a token a suite waits for, such as `AHCIPROBE OK`. A line
 * the debug shell prints is one console write too: the banner's
 * `Type 'help'` is what every suite waits for before it types, and a USB
 * stick's record is raised in an interrupt. The record is raised at the
 * line's seventh byte, the `h`.
 */
static void console_write_rows(void)
{
    char text[CAP];

    /* A record raised in the middle of a program's console write. */
    reset();
    console_write_begin();
    console_putc_raw('A');
    klog(KLOG_WARN, "block", "completion dropped: stale generation");
    ring_text(text, sizeof(text));
    CHECK("the ring takes the record where it is raised, inside the write",
          streq(text, "<4>[0.000000] block: completion dropped: stale generation\n"));
    CHECK("and the port has only the write's first byte so far",
          serial_len == 1 && serial[0] == 'A');
    console_putc_raw('B');
    console_putc_raw('\r');
    console_putc_raw('\n');
    console_write_end();
    serial[serial_len] = '\0';
    CHECK("the write's bytes reach the port together and the record after them",
          streq(serial, "AB\r\n<4>[0.000000] block: completion dropped: stale generation\r\n"));
    CHECK("and nothing waits or is held once the write has ended",
          pending_count() == 0 && klog_delivering == 0 && console_writing == 0);

    /* A record raised in the middle of a line the debug shell prints. */
    reset();
    reenter_after = 7;
    console_puts("Type 'help' for available commands.");
    serial[serial_len] = '\0';
    CHECK("a record raised inside a line of the shell follows the line on the port",
          streq(serial, "Type 'help' for available commands.\r\n"
                        "<6>[0.000000] usbdisk: ready: device 4: lun 0\r\n"));
    screen[screen_len] = '\0';
    CHECK("and on the screen", streq(screen, "Type 'help' for available commands.\n"
                                             "usbdisk: ready: device 4: lun 0\n"));
    CHECK("and nothing waits or is held once the line is out",
          pending_count() == 0 && klog_delivering == 0 && console_writing == 0);
    reset();
    reenter_after = 3;
    console_printf("%s %d\n", "USBDISKPROBE", 5);
    serial[serial_len] = '\0';
    CHECK("a record raised inside a formatted line of the shell follows it",
          streq(serial, "USBDISKPROBE 5\r\n"
                        "<6>[0.000000] usbdisk: ready: device 4: lun 0\r\n"));
    CHECK("and nothing waits or is held once the formatted line is out",
          pending_count() == 0 && klog_delivering == 0 && console_writing == 0);

    /* A full burst raised while the drain at a write's end writes the
     * queue's last record, which keeps its slot. */
    reset();
    console_write_begin();
    klog(KLOG_INFO, "outer", "began: queued behind a console write");
    reenter_times = KLOG_PENDING_MAX;
    console_write_end();
    screen[screen_len] = '\0';
    CHECK("the queue takes all of a burst beside the record in flight",
          reenter_times == 0 && count_lines(screen) == KLOG_PENDING_MAX + 1 &&
              !has(screen, "klog: dropped:") && pending_lost == 0);
    CHECK("and nothing waits after", pending_count() == 0 && pending_writing == 0);
}

/** The brackets of the interrupts a record takes, and a read of the ring. */
static void bracket_rows(void)
{
    char text[CAP];

    reset();
    klog(KLOG_INFO, "b", "check: the brackets");
    CHECK("a record takes three brackets", irq_pairs == 3);
    ring_text(text, sizeof(text));
    CHECK("and a read takes one more", irq_pairs == 4);
    CHECK("and leaves none open", irq_depth == 0 && irq_unbalanced == 0);
}

/**
 * The panic's first record, made with the queue full of records a console
 * write held back: the queue goes out first, and the record after it, not
 * dropped behind it; a record kept behind the full queue goes out between
 * the two; and a record a drain the panic stopped was part way through goes
 * out again whole to the destination that was writing it and those after,
 * and not to those that had it.
 */
static void panic_queue_rows(void)
{
    char text[CAP];
    int i;

    reset();
    console_write_begin();
    for (i = 0; i < KLOG_PENDING_MAX; i++) {
        klog(KLOG_INFO, "usb", "gone: device %d", i);
    }
    CHECK("the console write holds the queue full", pending_count() == KLOG_PENDING_MAX);
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: with the queue full");
    serial[serial_len] = '\0';
    CHECK("the panic's record reaches the port after the queue",
          has(serial, "usb: gone: device 63\r\n"
                      "<2>[0.000000] panic: stopped: with the queue full\r\n"));
    CHECK("and nothing is dropped or waits", pending_count() == 0 && pending_lost == 0);

    /* A record kept while the full queue waits. */
    reset();
    console_write_begin();
    for (i = 0; i < KLOG_PENDING_MAX; i++) {
        klog(KLOG_INFO, "usb", "gone: device %d", i);
    }
    host_cpu = 1;
    klog(KLOG_ERR, "nmi", "kept: before the panic");
    host_cpu = 0;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: after a kept record");
    ring_text(text, sizeof(text));
    CHECK("a record kept behind the full queue reaches the ring ahead of the panic's",
          has(text, "usb: gone: device 63\n"
                    "<3>[0.000000] nmi: kept: before the panic\n"
                    "<2>[0.000000] panic: stopped: after a kept record\n"));
    serial[serial_len] = '\0';
    CHECK("and the port, after the queue",
          has(serial, "usb: gone: device 63\r\n"
                      "<3>[0.000000] nmi: kept: before the panic\r\n"
                      "<2>[0.000000] panic: stopped: after a kept record\r\n"));
    CHECK("and nothing is kept, waits or is dropped",
          !klog_kept() && pending_count() == 0 && pending_lost == 0);

    /* A drain stopped at the fifth byte of its first record. */
    reset();
    console_write_begin();
    klog(KLOG_INFO, "usb", "gone: device 0");
    klog(KLOG_INFO, "usb", "gone: device 1");
    stop_at_serial = 5;
    if (setjmp(stopped) == 0) {
        console_write_end();
    }
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: inside a drain");
    serial[serial_len] = '\0';
    CHECK("a record a stopped drain was writing goes out again whole, the rest once",
          streq(serial, "<6>[0"
                        "<6>[0.000000] usb: gone: device 0\r\n"
                        "<6>[0.000000] usb: gone: device 1\r\n"
                        "<2>[0.000000] panic: stopped: inside a drain\r\n"));
    screen[screen_len] = '\0';
    CHECK("and the screen, which had it whole, has it once",
          streq(screen, "usb: gone: device 0\n"
                        "usb: gone: device 1\n"
                        "panic: stopped: inside a drain\n"));
    CHECK("and nothing waits after", pending_count() == 0);

    /* A drain stopped at the fifth byte netconsole had of its first record. */
    reset();
    net_on = 1;
    console_write_begin();
    klog(KLOG_INFO, "usb", "gone: device 0");
    klog(KLOG_INFO, "usb", "gone: device 1");
    stop_at_net = 5;
    if (setjmp(stopped) == 0) {
        console_write_end();
    }
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: inside netconsole");
    net[net_len] = '\0';
    CHECK("netconsole, which was writing it, has the record again whole",
          streq(net, "<6>[0"
                     "<6>[0.000000] usb: gone: device 0\n"
                     "<6>[0.000000] usb: gone: device 1\n"
                     "<2>[0.000000] panic: stopped: inside netconsole\n"));
    serial[serial_len] = '\0';
    CHECK("and the port, which had it whole, has it once",
          streq(serial, "<6>[0.000000] usb: gone: device 0\r\n"
                        "<6>[0.000000] usb: gone: device 1\r\n"
                        "<2>[0.000000] panic: stopped: inside netconsole\r\n"));
    screen[screen_len] = '\0';
    CHECK("and so has the screen", streq(screen, "usb: gone: device 0\n"
                                                 "usb: gone: device 1\n"
                                                 "panic: stopped: inside netconsole\n"));
    CHECK("and nothing waits after", pending_count() == 0);
}

/**
 * A record a drain the panic stopped was part way through goes out again
 * from its own slot, which a full burst raised during its write leaves
 * alone: with no slot beyond the queue's limit for the record in flight,
 * the burst's last record would take its place.
 */
static void panic_burst_rows(void)
{
    reset();
    console_write_begin();
    klog(KLOG_INFO, "outer", "began: queued behind a console write");
    reenter_times = KLOG_PENDING_MAX;
    stop_at_serial = 5;
    if (setjmp(stopped) == 0) {
        console_write_end();
    }
    CHECK("the queue holds the burst beside the record in flight",
          reenter_times == 0 && pending_count() == KLOG_PENDING_MAX + 1 && pending_lost == 0);
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: after a burst");
    serial[serial_len] = '\0';
    CHECK("the panic writes the record in flight out again whole, then the burst",
          has(serial, "<6>[0"
                      "<6>[0.000000] outer: began: queued behind a console write\r\n"
                      "<3>[0.000000] irq: landed: one of a burst\r\n") &&
              count_lines(serial) == KLOG_PENDING_MAX + 2 &&
              has(serial, "irq: landed: one of a burst\r\n"
                          "<2>[0.000000] panic: stopped: after a burst\r\n"));
    screen[screen_len] = '\0';
    CHECK("and the screen, which had it whole, has it once",
          has(screen, "outer: began: queued behind a console write\n"
                      "irq: landed: one of a burst\n") &&
              count_lines(screen) == KLOG_PENDING_MAX + 2);
    CHECK("and nothing waits or is dropped after",
          pending_count() == 0 && pending_lost == 0 && !has(serial, "klog: dropped:"));
}

/**
 * The records kept for later, made in an NMI or by a processor without the
 * big lock: they reach the ring and the destinations in the order they were
 * made, ahead of a record made after them, inside another record's delivery
 * too; and a slot a stopped processor left part built holds back the
 * records after it until a panic skips it.
 */
static void kept_rows(void)
{
    char text[CAP];

    /* Kept on one processor, then a record of the holder's. */
    reset();
    host_cpu = 1;
    klog(KLOG_ERR, "watchdog", "first: kept");
    klog(KLOG_ERR, "watchdog", "second: kept");
    CHECK("a record without the big lock does not reach the ring", klog_len() == 0);
    CHECK("and is kept", klog_kept());
    host_cpu = 0;
    klog(KLOG_INFO, "holder", "next: a record of the holder's");
    ring_text(text, sizeof(text));
    CHECK("the kept records reach the ring first, in the order they were made",
          streq(text, "<3>[0.000000] watchdog: first: kept\n"
                      "<3>[0.000000] watchdog: second: kept\n"
                      "<6>[0.000000] holder: next: a record of the holder's\n"));
    serial[serial_len] = '\0';
    CHECK("and the port the same",
          streq(serial, "<3>[0.000000] watchdog: first: kept\r\n"
                        "<3>[0.000000] watchdog: second: kept\r\n"
                        "<6>[0.000000] holder: next: a record of the holder's\r\n"));
    CHECK("and none is kept or waits after", !klog_kept() && pending_count() == 0);

    /* Kept, then a record made, both inside another record's delivery. */
    reset();
    kept_then_nested = 1;
    klog(KLOG_INFO, "outer", "began: the record being delivered");
    ring_text(text, sizeof(text));
    CHECK("inside a delivery, the kept record reaches the ring ahead of the one "
          "made after it",
          streq(text, "<6>[0.000000] outer: began: the record being delivered\n"
                      "<3>[0.000000] nmi: kept: made in an NMI\n"
                      "<3>[0.000000] irq: landed: after the kept one\n"));
    serial[serial_len] = '\0';
    CHECK("and the port the same",
          streq(serial, "<6>[0.000000] outer: began: the record being delivered\r\n"
                        "<3>[0.000000] nmi: kept: made in an NMI\r\n"
                        "<3>[0.000000] irq: landed: after the kept one\r\n"));

    /* A slot a processor stopped by the panic left part built. */
    reset();
    host_cpu = 1;
    klog(KLOG_ERR, "a", "kept: before the part-built slot");
    defer_tail++; /* a writer took the slot and stopped */
    klog(KLOG_ERR, "b", "kept: after the part-built slot");
    host_cpu = 0;
    klog(KLOG_INFO, "c", "made: by the holder");
    ring_text(text, sizeof(text));
    CHECK("a part-built slot holds back the kept records after it",
          streq(text, "<3>[0.000000] a: kept: before the part-built slot\n"
                      "<6>[0.000000] c: made: by the holder\n") &&
              klog_kept());
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: the row's panic");
    ring_text(text, sizeof(text));
    CHECK("the panic skips it and writes the rest out ahead of its own",
          streq(text, "<3>[0.000000] a: kept: before the part-built slot\n"
                      "<6>[0.000000] c: made: by the holder\n"
                      "<3>[0.000000] b: kept: after the part-built slot\n"
                      "<2>[0.000000] panic: stopped: the row's panic\n") &&
              !klog_kept());
}

/**
 * The records kept for later, when there are more than the queue holds,
 * which counts the ones it drops; and when the holder of the big lock is
 * locked up and another processor writes them out (`klog_flush_stuck`),
 * which stands back while the holder is in the middle of a record, while
 * the holder, back while it writes them, keeps its own for later.
 */
static void kept_stuck_rows(void)
{
    char text[CAP];
    int i;

    /* Two more kept than the queue holds. */
    reset();
    host_cpu = 1;
    for (i = 0; i < KLOG_DEFER_MAX + 2; i++) {
        klog(KLOG_ERR, "nmi", "kept: %d", i);
    }
    host_cpu = 0;
    klog(KLOG_INFO, "holder", "next: after the full queue");
    ring_text(text, sizeof(text));
    CHECK("a full queue keeps the records it has room for", has(text, "nmi: kept: 0\n") &&
                                                                has(text, "nmi: kept: 127\n") &&
                                                                !has(text, "nmi: kept: 128\n"));
    CHECK("and counts the ones it dropped after them, ahead of the holder's record",
          has(text, "nmi: kept: 127\n"
                    "<4>[0.000000] klog: dropped: 2 record(s) the queue could not hold\n"
                    "<6>[0.000000] holder: next: after the full queue\n"));
    CHECK("and every one reaches the ring once",
          count_lines(text) == KLOG_DEFER_MAX + 2 && !klog_kept());

    /* Another processor writes them out for a holder locked up. */
    reset();
    host_cpu = 1;
    klog(KLOG_ERR, "watchdog", "report: of the holder");
    klog_users = 1; /* the holder is in the middle of a record */
    klog_flush_stuck();
    CHECK("the writer stands back while the holder is in a record",
          klog_len() == 0 && klog_kept() && defer_taking == 0);
    klog_users = 0;
    holder_at_serial = 1;
    klog_flush_stuck();
    ring_text(text, sizeof(text));
    CHECK("otherwise it writes them out, and a holder back meanwhile keeps its own",
          streq(text, "<3>[0.000000] watchdog: report: of the holder\n") && klog_kept());
    serial[serial_len] = '\0';
    CHECK("and the port has only the report",
          streq(serial, "<3>[0.000000] watchdog: report: of the holder\r\n"));
    CHECK("and lets the state go when done",
          defer_taking == 0 && klog_delivering == 0 && pending_count() == 0);
    host_cpu = 0;
    klog(KLOG_INFO, "holder", "next: after the writer is done");
    ring_text(text, sizeof(text));
    CHECK("the holder's next record writes its kept one out ahead of itself",
          streq(text, "<3>[0.000000] watchdog: report: of the holder\n"
                      "<3>[0.000000] holder: back: while another writes the kept records\n"
                      "<6>[0.000000] holder: next: after the writer is done\n"));
    CHECK("and the holder leaves klog as it entered", klog_users == 0);
}

/**
 * Two processors that find the holder locked up: the second leaves the
 * first's number in `defer_taking`; a nested record of the holder's stays
 * in while a writer has exchanged it but not yet stood back; and the
 * writer takes only the records kept when it began, over the second pass
 * that more than the queue holds takes.
 */
static void flusher_rows(void)
{
    char text[CAP];
    int i;

    /* A second writer while processor 1 writes them out. */
    reset();
    host_cpu = 1;
    klog(KLOG_ERR, "watchdog", "report: of the holder");
    defer_taking = 2; /* processor 1 is writing the kept records out */
    host_cpu = 2;
    klog_flush_stuck();
    CHECK("a second writer leaves the first's number and writes nothing",
          defer_taking == 2 && klog_len() == 0 && klog_kept());
    host_cpu = 0;
    klog(KLOG_INFO, "holder", "next: while processor 1 writes");
    CHECK("and the holder still keeps its record for later", klog_len() == 0 && klog_users == 0);

    /* A nested record of the holder's, after a writer's exchange. */
    reset();
    klog_users = 1; /* the holder is in the middle of a record */
    defer_taking = 2;
    klog(KLOG_ERR, "irq", "landed: inside the holder's record");
    CHECK("a nested record stays in whatever defer_taking says",
          klog_len() > 0 && !klog_kept() && klog_users == 1);
    klog_users = 0;

    /* One more kept than the queue holds, and one kept while the writer
     * writes the first pass, which the second pass leaves for the next. */
    reset();
    host_cpu = 1;
    for (i = 0; i <= KLOG_PENDING_MAX; i++) {
        klog(KLOG_ERR, "watchdog", "report: line %d", i);
    }
    kept_at_serial = 1;
    klog_flush_stuck();
    ring_text(text, sizeof(text));
    CHECK("the writer takes only the records kept when it began",
          has(text, "watchdog: report: line 64\n") && !has(text, "nmi: kept") &&
              count_lines(text) == KLOG_PENDING_MAX + 1 && klog_kept());
    host_cpu = 0;
    klog(KLOG_INFO, "holder", "next: after the writer");
    ring_text(text, sizeof(text));
    CHECK("and the holder writes the later one out ahead of its own",
          has(text, "<3>[0.000000] nmi: kept: while another writes the kept records\n"
                    "<6>[0.000000] holder: next: after the writer\n"));
}

/**
 * The console's bytes inside klog's exclusion: the writer stands back
 * while the holder is inside a console write, and the holder's byte, made
 * while another processor writes the kept records out, waits for it to
 * finish; the holder is not switched out meanwhile (`klog_inside`).
 */
static void console_exclusion_rows(void)
{
    reset();
    console_write_begin();
    CHECK("a console write holds klog's state", klog_inside() && klog_users == 1);
    host_cpu = 1;
    klog(KLOG_ERR, "watchdog", "report: of the holder");
    klog_flush_stuck();
    CHECK("and the writer stands back inside it", klog_len() == 0 && defer_taking == 0);
    host_cpu = 0;
    console_putc_raw('A');
    console_write_end();
    CHECK("the write's end lets klog's state go", !klog_inside() && klog_users == 0);

    reset();
    defer_taking = 2; /* processor 1 is writing the kept records out */
    relax_frees = 1;
    console_putc('x');
    serial[serial_len] = '\0';
    CHECK("a byte of the holder's waits for the writer to finish",
          relax_turns > 0 && serial_at_relax == 0 && streq(serial, "x"));
    CHECK("and lets klog's state go after it", klog_users == 0);

    reset();
    defer_taking = 2;
    relax_frees = 1;
    console_write_begin();
    CHECK("a console write begun meanwhile waits too",
          relax_turns > 0 && defer_taking == 0 && klog_users == 1);
    console_write_end();
    CHECK("and ends with klog's state let go", klog_users == 0 && console_writing == 0);
}

/**
 * A panic that stops a writer part way: the ring keeps the state from
 * before the append it stopped, and a kept record a stopped collector had
 * put in the ring and the queue goes out once.
 */
static void panic_append_rows(void)
{
    char text[CAP];
    struct ring_state *st;
    const char *part = "<6>[0.000000] torn: never committed\n";
    unsigned i;

    /* Stopped after the eviction, part way through the bytes. */
    reset();
    klog(KLOG_INFO, "before", "made: whole");
    st = ring_next();
    ring_evict(st, 36);
    ring_commit();
    for (i = 0; i < 20; i++) {
        ring[(st->head + i) % KLOG_RING_SIZE] = part[i];
    }
    ring_lock = 1; /* held by the processor the panic stopped */
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: inside an append");
    ring_text(text, sizeof(text));
    CHECK("a panic drops the append it stopped and writes after the last whole record",
          streq(text, "<6>[0.000000] before: made: whole\n"
                      "<2>[0.000000] panic: stopped: inside an append\n"));

    /* A full ring, stopped inside the eviction's bytes. */
    reset();
    for (i = 0; i < 400; i++) {
        klog(KLOG_DEBUG, "fill", "record: %d of two thousand", (int)i);
    }
    st = ring_next();
    ring_evict(st, 200);
    ring_commit();
    for (i = 0; i < 150; i++) {
        ring[(st->head + i) % KLOG_RING_SIZE] = 'x';
    }
    ring_lock = 1;
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: inside an append to a full ring");
    ring_text(text, sizeof(text));
    CHECK("a full ring stopped part way still begins and ends at a record",
          text[0] == '<' && !has(text, "x") &&
              has(text, "record: 399 of two thousand\n"
                        "<2>[0.000000] panic: stopped: inside an append to a full ring\n"));

    /* A collector stopped after the ring and the queue took a kept record. */
    reset();
    host_cpu = 1;
    klog(KLOG_ERR, "nmi", "kept: before the stop");
    host_cpu = 0;
    ring_append(deferred[0].text, deferred[0].len, 1u);
    pending_put(&deferred[0], 1u);
    panicking = 1;
    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: inside a collect");
    ring_text(text, sizeof(text));
    CHECK("the kept record is in the ring once",
          streq(text, "<3>[0.000000] nmi: kept: before the stop\n"
                      "<2>[0.000000] panic: stopped: inside a collect\n"));
    serial[serial_len] = '\0';
    CHECK("and on the port once",
          streq(serial, "<3>[0.000000] nmi: kept: before the stop\r\n"
                        "<2>[0.000000] panic: stopped: inside a collect\r\n"));
}

/**
 * A record made inside a console write while kept records wait for room in
 * a full queue is kept behind them, and the ring has it after them.
 */
static void nested_order_rows(void)
{
    char text[CAP];
    int i;

    reset();
    console_write_begin();
    for (i = 0; i < KLOG_PENDING_MAX; i++) {
        klog(KLOG_INFO, "usb", "gone: device %d", i);
    }
    host_cpu = 1;
    klog(KLOG_ERR, "nmi", "kept: behind the full queue");
    host_cpu = 0;
    klog(KLOG_ERR, "irq", "landed: after the kept one");
    CHECK("the nested record is kept behind the one the queue could not take",
          klog_kept() && pending_lost == 0);
    console_write_end();
    ring_text(text, sizeof(text));
    CHECK("the ring has the kept record ahead of the nested one",
          has(text, "usb: gone: device 63\n"
                    "<3>[0.000000] nmi: kept: behind the full queue\n"
                    "<3>[0.000000] irq: landed: after the kept one\n"));
    serial[serial_len] = '\0';
    CHECK("and so has the port", has(serial, "usb: gone: device 63\r\n"
                                             "<3>[0.000000] nmi: kept: behind the full queue\r\n"
                                             "<3>[0.000000] irq: landed: after the kept one\r\n"));
    CHECK("and nothing is kept, waits or is dropped",
          !klog_kept() && pending_count() == 0 && pending_lost == 0);
}

/** The rows of klog's exclusion, its ring's commit and its kept order. */
static void exclusion_rows(void)
{
    flusher_rows();
    console_exclusion_rows();
    panic_append_rows();
    nested_order_rows();
}

/**
 * Run the rows; the count of failures, at most 255.
 *
 * A record is the only place most of the formatter's conversions are used, so
 * the rows check every one. A record is one line: the ring evicts by finding a
 * newline, so a record with one of its own would be dropped in halves, leaving
 * a tail with no header, and a newline inside the text is kept as a space. The
 * formatter reads the byte after a `%`, which for a format that ends on its
 * percent sign is past the end of the string. A level out of range is clamped
 * rather than written into the prefix, where a reader parses a single digit.
 *
 * netconsole is given records before it has a device to send through: it holds
 * what it cannot send yet and sends it when a device arrives, and the records
 * worth having on the wire are the early ones.
 *
 * The ring's contents after 2000 records are rebuilt from the records
 * themselves, since a ring that cleared itself on overflow, or dropped one
 * record too many, holds whole lines too and would pass the other eviction
 * checks. A ring that dropped bytes rather than records would leave a line
 * beginning in the middle of a number.
 *
 * A record raised from inside the delivery of another, which is what an
 * interrupt handler that prints does, tears neither, and both reach every
 * destination past the ring, the one raised from inside after the line it
 * landed in: an unplugged device says so once, and a machine that was writing
 * at that moment would otherwise never say it on its port. Of more such records
 * than the queue holds, the eight it holds are printed in order and the rest
 * are counted in a record of their own, so that the log says how many it lost.
 *
 * A record takes three brackets of the interrupts, one for the build, one for
 * the ring append and one for the look at the queue, and a read of the ring
 * takes one more.
 */
int main(void)
{
    char text[CAP];
    unsigned i;

    /* A record: priority, uptime, subsystem, text and one newline. */
    reset();
    kernel_ticks = 1234u * (TICK_HZ / 100u);
    klog(KLOG_ERR, "ahci", "command: timed out on port %d", 0);
    ring_text(text, sizeof(text));
    CHECK("the record is the whole line",
          streq(text, "<3>[12.340000] ahci: command: timed out on port 0\n"));

    /* Every conversion the kernel's formatter has. */
    reset();
    klog(KLOG_INFO, "fmt", "%d %b %o %x %X %s %c %%", -5, 5, 8, 255, 255, "text", 'z');
    ring_text(text, sizeof(text));
    CHECK("the conversions are the kernel's own",
          streq(text, "<6>[0.000000] fmt: -5 101 10 ff FF text z %\n"));

    /* A null string is the formatter's, not a fault. */
    reset();
    klog(KLOG_INFO, "fmt", "name: %s", (char *)0);
    ring_text(text, sizeof(text));
    CHECK("a null string prints as the formatter renders it", has(text, "name: (null)\n"));

    /* The timestamp before the timer starts. */
    reset();
    klog(KLOG_INFO, "boot", "early: no clock yet");
    ring_text(text, sizeof(text));
    CHECK("a record before the first tick reads zero", has(text, "<6>[0.000000] boot: "));

    /* Microseconds are six digits, so a reader takes them as Linux's. */
    reset();
    kernel_ticks = 205u * (TICK_HZ / 100u);
    klog(KLOG_INFO, "t", "x: y");
    ring_text(text, sizeof(text));
    CHECK("the fraction keeps its leading zero", has(text, "[2.050000] "));

    /* One newline, however the format ended. */
    reset();
    klog(KLOG_INFO, "n", "a: b\n");
    ring_text(text, sizeof(text));
    CHECK("a format that ends in a newline still gives one",
          streq(text, "<6>[0.000000] n: a: b\n"));
    reset();
    klog(KLOG_INFO, "n", "a: b\n\n\n");
    ring_text(text, sizeof(text));
    CHECK("and one that ends in three gives one", streq(text, "<6>[0.000000] n: a: b\n"));

    /* Truncation is marked, and the record still ends in one newline. */
    reset();
    klog(KLOG_INFO, "long",
         "text: 012345678901234567890123456789012345678901234567890123456789"
         "012345678901234567890123456789012345678901234567890123456789"
         "012345678901234567890123456789012345678901234567890123456789"
         "012345678901234567890123456789012345678901234567890123456789");
    i = ring_text(text, sizeof(text));
    CHECK("a record too long for the buffer fits in it", i <= KLOG_RECORD_MAX);
    CHECK("and says it was cut", has(text, "...\n"));
    CHECK("and ends in exactly one newline", text[i - 1] == '\n' && text[i - 2] != '\n');
    CHECK("and is one record", count_lines(text) == 1);

    /* One record is one line. */
    reset();
    klog(KLOG_INFO, "nl", "text: %s", "first\nsecond");
    ring_text(text, sizeof(text));
    CHECK("a newline inside the text does not end the record", count_lines(text) == 1);
    CHECK("and is kept as a space", streq(text, "<6>[0.000000] nl: text: first second\n"));

    /* A format that ends on its own percent sign. */
    reset();
    klog(KLOG_INFO, "pct", "text: 100%");
    ring_text(text, sizeof(text));
    CHECK("a format ending in a percent sign stops there",
          streq(text, "<6>[0.000000] pct: text: 100%\n"));

    /* A level out of range is clamped. */
    reset();
    klog(99, "clamp", "level: too large");
    ring_text(text, sizeof(text));
    CHECK("a level past the last is clamped", has(text, "<7>"));
    reset();
    klog(-1, "clamp", "level: too small");
    ring_text(text, sizeof(text));
    CHECK("a level before the first is clamped", has(text, "<2>"));

    /* The screen takes INFO and above, and takes it without the prefix. */
    reset();
    klog(KLOG_DEBUG, "d", "detail: for the log");
    CHECK("debug detail does not reach the screen", screen_len == 0);
    CHECK("but it is in the ring", klog_len() > 0);
    reset();
    klog(KLOG_INFO, "i", "event: for a person");
    screen[screen_len] = '\0';
    CHECK("an informational record reaches the screen", streq(screen, "i: event: for a person\n"));
    CHECK("without the priority and the timestamp", !has(screen, "<6>"));
    CHECK("and held the cursor once for its bytes", holds == 1 && releases == 1);

    /* The serial port takes every level, and netconsole only when it is on. */
    reset();
    klog(KLOG_DEBUG, "d", "detail: for the log");
    serial[serial_len] = '\0';
    CHECK("debug detail reaches the serial port whole",
          streq(serial, "<7>[0.000000] d: detail: for the log\r\n"));
    CHECK("and not the wire while netconsole is off", net_len == 0);
    reset();
    net_on = 1;
    klog(KLOG_INFO, "i", "event: happened");
    net[net_len] = '\0';
    CHECK("netconsole gets the whole record", streq(net, "<6>[0.000000] i: event: happened\n"));
    /* netconsole is given records before it has a device. */
    reset();
    net_on = 0;
    klog(KLOG_INFO, "early", "event: before the network");
    CHECK("and is given records before it has a device", net_calls > 0);

    /* The order, and that a wedged port does not take the record with it. */
    reset();
    net_on = 1;
    serial_wedged = 1;
    klog(KLOG_INFO, "order", "check: the four in turn");
    order[order_len] = '\0';
    CHECK("the screen is written before the serial port and the wire", streq(order, "SUN"));
    ring_text(text, sizeof(text));
    CHECK("the ring already held the record when the screen was written",
          klog_len() > 0 && ring_at_screen == klog_len());
    CHECK("a port that never drains does not cost the ring its copy",
          has(text, "order: check: the four in turn"));
    CHECK("and the wedged port wrote nothing", serial_len == 0);
    CHECK("though every byte was offered to it", serial_offered == klog_len() + 1);

    /* Eviction: the ring drops whole records, so it never begins mid-line. */
    reset();
    for (i = 0; i < 2000; i++) {
        klog(KLOG_DEBUG, "fill", "record: %d of two thousand", (int)i);
    }
    CHECK("the ring holds no more than its size", klog_len() <= KLOG_RING_SIZE);
    CHECK("and no less than it can, within one record", klog_len() > KLOG_RING_SIZE - 64);
    ring_text(text, sizeof(text));
    CHECK("what it holds begins at a record", text[0] == '<');
    CHECK("and ends at one", text[klog_len() - 1] == '\n');
    CHECK("the oldest records were dropped", !has(text, "record: 0 of"));
    CHECK("the newest was kept", has(text, "record: 1999 of"));
    {
        /* The ring holds the newest records and only those. */
        char want[KLOG_RING_SIZE + 1];
        char line[128];
        unsigned wlen = 0;
        int oldest = 1999;

        while (oldest >= 0) {
            unsigned n = render_fill(line, sizeof(line), oldest);

            if (wlen + n > klog_len()) {
                break;
            }
            wlen += n;
            oldest--;
        }
        wlen = 0;
        for (i = (unsigned)(oldest + 1); i < 2000; i++) {
            unsigned n = render_fill(line, sizeof(line), (int)i);
            unsigned j;

            for (j = 0; j < n; j++) {
                want[wlen++] = line[j];
            }
        }
        want[wlen] = '\0';
        CHECK("the ring holds exactly the newest records, byte for byte",
              wlen == klog_len() && streq(text, want));
    }

    {
        /* Every line in the ring is a prefix, a subsystem and a body. */
        const char *p = text;
        unsigned whole = 0, lines = 0;

        while (*p != '\0') {
            const char *nl = p;

            while (*nl != '\0' && *nl != '\n') {
                nl++;
            }
            lines++;
            if (p[0] == '<' && p[2] == '>' && p[3] == '[') {
                whole++;
            }
            p = (*nl == '\n') ? nl + 1 : nl;
        }
        CHECK("every line the ring holds is a whole record", whole == lines);
    }

    /* The console is not the log. */
    reset();
    console_puts("Type 'help' for the command list");
    CHECK("console output reaches the screen", screen_len > 0);
    CHECK("and the serial port", serial_len > 0);
    CHECK("and never the ring", klog_len() == 0);
    reset();
    net_on = 1;
    console_puts("shell> ls");
    CHECK("and never the wire, even with netconsole on", net_len == 0);
    screen[screen_len] = '\0';
    CHECK("the console writes what it was given", streq(screen, "shell> ls\n"));
    serial[serial_len] = '\0';
    CHECK("and expands the newline for a terminal", streq(serial, "shell> ls\r\n"));

    /* A record raised from inside the delivery of another. */
    reset();
    net_on = 1;
    reenter_from_screen = 1;
    klog(KLOG_INFO, "outer", "began: before the interrupt");
    ring_text(text, sizeof(text));
    CHECK("both records are in the ring", count_lines(text) == 2);
    CHECK("the one raised from inside is whole",
          has(text, "<3>[0.000000] irq: landed: in the middle of another record\n"));
    CHECK("and so is the one it interrupted",
          has(text, "<6>[0.000000] outer: began: before the interrupt\n"));
    /* The one raised from inside waits for the line it landed in. */
    screen[screen_len] = '\0';
    CHECK("the screen has the record that was interrupted, then the one that "
          "interrupted it",
          streq(screen, "outer: began: before the interrupt\n"
                        "irq: landed: in the middle of another record\n"));
    serial[serial_len] = '\0';
    CHECK("and so does the serial port",
          streq(serial, "<6>[0.000000] outer: began: before the interrupt\r\n"
                        "<3>[0.000000] irq: landed: in the middle of another record\r\n"));
    net[net_len] = '\0';
    CHECK("and so does the wire",
          streq(net, "<6>[0.000000] outer: began: before the interrupt\n"
                     "<3>[0.000000] irq: landed: in the middle of another record\n"));

    /* More records raised from inside one delivery than the queue holds. */
    reset();
    reenter_times = KLOG_PENDING_MAX + 1;
    klog(KLOG_INFO, "outer", "began: before the burst");
    CHECK("the burst is spent", reenter_times == 0);
    ring_text(text, sizeof(text));
    CHECK("the ring has the record, the burst raised inside it, and the count",
          count_lines(text) == KLOG_PENDING_MAX + 3);
    CHECK("and the count says how many were dropped",
          has(text, "<4>[0.000000] klog: dropped: 1 record(s) the queue could not hold\n"));
    screen[screen_len] = '\0';
    CHECK("the screen has the record, all of the burst the queue holds, and the count",
          count_lines(screen) == KLOG_PENDING_MAX + 2);
    CHECK("the screen begins with the record that was interrupted",
          has(screen, "outer: began: before the burst\n"
                      "irq: landed: one of a burst\n"));
    CHECK("and ends with the count",
          has(screen, "klog: dropped: 1 record(s) the queue could not hold\n"));

    console_write_rows();
    kept_rows();

    kept_stuck_rows();
    exclusion_rows();
    panic_queue_rows();
    panic_burst_rows();
    bracket_rows();

    if (fails == 0) {
        printf("klog_hosttest: PASS\n");
    }
    return fails > 255 ? 255 : fails;
}
