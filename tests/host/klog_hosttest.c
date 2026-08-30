/**
 * The kernel log, driven on the host (sys/klog.c).
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * klog.c is included rather than linked, so each case can put the ring and the
 * recorders back to their initial state; the kernel has one boot per run and
 * needs no such thing. The four destinations are replaced by recorders, which
 * is what lets the cases here be the ones a booting machine will not produce
 * on demand: a ring that wraps mid-record, a serial port that never becomes
 * ready, a record longer than the buffer, and a record raised from inside the
 * delivery of another, which is what an interrupt handler that prints does.
 *
 * The records are raised through `klog_v` rather than `klog`. Reaching a
 * variadic function's arguments by walking the stack from the last named one
 * is true on i386 and not on the machine this test compiles for, so the words
 * are handed over as an array: one per argument, in order, which is the same
 * thing the kernel's own call builds.
 *
 * Two things are checked about interrupts, and one of them is checked
 * indirectly. That a re-entrant record does not tear the one being delivered
 * is checked outright, by re-entering from the screen recorder. That the
 * formatter's shared sink is protected is checked by counting the brackets:
 * one klog call takes exactly two, the build and the ring append, and they
 * balance. A host has no interrupts to land in the middle, so the count is
 * what stands in for the case that cannot be staged here.
 */

/* The kernel headers define the fixed-width types themselves, so nothing from
   the host's stdint may be included alongside them. */
extern int printf(const char *fmt, ...);

#include "../../rach2/sys/klog.h"

/* One argument word, the way the kernel's own call would push it: an integer
   in the low bytes of a stack slot, which is where `k_vprintf` reads it. */
#define WORD(v) ((char *)(long)(v))

static int fails;

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

static int serial_wedged;       /* the port never becomes ready */
static int net_on;              /* netconsole is enabled */
static int reenter_from_screen; /* raise a record from inside delivery */
static int irq_depth;
static int irq_pairs;
static int irq_unbalanced;

/* Which destination wrote, in the order they were first written to. Counted
   per destination and not per byte, so a destination that accepted nothing
   (a wedged port) is still recorded as having been offered the record. */
static unsigned screen_calls, serial_calls, net_calls;

static void note(char c)
{
    if (order_len < sizeof(order) - 1) {
        order[order_len++] = c;
    }
}

void k_putc(char c)
{
    if (screen_calls++ == 0) {
        note('S');
    }
    if (screen_len < CAP) {
        screen[screen_len++] = c;
    }
    if (reenter_from_screen) {
        reenter_from_screen = 0;
        klog(KLOG_ERR, "irq", "landed: in the middle of another record");
    }
}

void serial_out(char c)
{
    if (serial_calls++ == 0) {
        note('U');
    }
    if (serial_wedged) {
        return; /* the transmit wait gave up */
    }
    if (serial_len < CAP) {
        serial[serial_len++] = c;
    }
}

int netconsole_is_enabled(void)
{
    return net_on;
}

void netconsole_putc(char c)
{
    if (net_calls++ == 0) {
        note('N');
    }
    if (net_len < CAP) {
        net[net_len++] = c;
    }
}

/* The stubs the destinations resolve to. The colour setter takes the screen
   driver's own enumeration, so its header comes first and the definition
   follows the declaration it is standing in for. */
#include "../../rach2/sys/display/vga_text.h"

void set_fore_color(display_color_t color)
{
    (void)color;
}

unsigned int kirq_save(void)
{
    irq_depth++;
    return 0;
}

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

volatile unsigned int kernel_ticks;

#include "../../rach2/sys/klog.c"

/* ---- helpers ---- */

static void reset(void)
{
    unsigned i;

    screen_len = serial_len = net_len = order_len = 0;
    screen_calls = serial_calls = net_calls = 0;
    serial_wedged = 0;
    net_on = 0;
    reenter_from_screen = 0;
    irq_depth = irq_pairs = irq_unbalanced = 0;
    kernel_ticks = 0;
    ring_head = ring_used = ring_oldest = 0;
    for (i = 0; i < KLOG_RING_SIZE; i++) {
        ring[i] = 0;
    }
    screen[0] = serial[0] = net[0] = order[0] = '\0';
}

static unsigned ring_text(char *out, unsigned cap)
{
    unsigned n = klog_read(out, cap - 1, 0);

    out[n] = '\0';
    return n;
}

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

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

int main(void)
{
    char text[CAP];
    unsigned i;

    /* The shape of a record: the priority, the uptime, the subsystem, the
       text, and one newline. */
    reset();
    kernel_ticks = 1234;
    {
        char *args[1];

        args[0] = WORD(0);
        klog_v(KLOG_ERR, "ahci", "command: timed out on port %d", args);
    }
    ring_text(text, sizeof(text));
    CHECK("the record is the whole line",
          streq(text, "<3>[12.34] ahci: command: timed out on port 0\n"));

    /* Every conversion the kernel formatter has, since a record is the only
       place most of them are now used. */
    reset();
    {
        char *args[7];

        args[0] = WORD(-5);
        args[1] = WORD(5);
        args[2] = WORD(8);
        args[3] = WORD(255);
        args[4] = WORD(255);
        args[5] = (char *)"text";
        args[6] = WORD('z');
        klog_v(KLOG_INFO, "fmt", "%d %b %o %x %X %s %c %%", args);
    }
    ring_text(text, sizeof(text));
    CHECK("the conversions are the kernel's own",
          streq(text, "<6>[0.00] fmt: -5 101 10 ff FF text z %\n"));

    /* A null string is the formatter's, not a fault. */
    reset();
    {
        char *args[1];

        args[0] = (char *)0;
        klog_v(KLOG_INFO, "fmt", "name: %s", args);
    }
    ring_text(text, sizeof(text));
    CHECK("a null string prints as the formatter renders it", has(text, "name: (null)\n"));

    /* The timestamp before the timer starts. */
    reset();
    klog(KLOG_INFO, "boot", "early: no clock yet");
    ring_text(text, sizeof(text));
    CHECK("a record before the first tick reads zero", has(text, "<6>[0.00] boot: "));

    /* Hundredths are two digits, so a reader can subtract them. */
    reset();
    kernel_ticks = 205;
    klog(KLOG_INFO, "t", "x: y");
    ring_text(text, sizeof(text));
    CHECK("the fraction keeps its leading zero", has(text, "[2.05] "));

    /* One newline, however the format ended. */
    reset();
    klog(KLOG_INFO, "n", "a: b\n");
    ring_text(text, sizeof(text));
    CHECK("a format that ends in a newline still gives one", streq(text, "<6>[0.00] n: a: b\n"));
    reset();
    klog(KLOG_INFO, "n", "a: b\n\n\n");
    ring_text(text, sizeof(text));
    CHECK("and one that ends in three gives one", streq(text, "<6>[0.00] n: a: b\n"));

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

    /* The level is what it was given, and out-of-range is clamped rather than
       written into the prefix, where a reader parses a single digit. */
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

    /* The serial port takes every level, and netconsole only when it is on. */
    reset();
    klog(KLOG_DEBUG, "d", "detail: for the log");
    serial[serial_len] = '\0';
    CHECK("debug detail reaches the serial port whole",
          streq(serial, "<7>[0.00] d: detail: for the log\r\n"));
    CHECK("and not the wire while netconsole is off", net_len == 0);
    reset();
    net_on = 1;
    klog(KLOG_INFO, "i", "event: happened");
    net[net_len] = '\0';
    CHECK("an enabled netconsole gets the whole record",
          streq(net, "<6>[0.00] i: event: happened\n"));

    /* The order, and that a wedged port does not take the record with it. */
    reset();
    net_on = 1;
    serial_wedged = 1;
    klog(KLOG_INFO, "order", "check: the four in turn");
    order[order_len] = '\0';
    CHECK("the screen is written before the serial port and the wire", streq(order, "SUN"));
    ring_text(text, sizeof(text));
    CHECK("a port that never drains does not cost the ring its copy",
          has(text, "order: check: the four in turn"));
    CHECK("and the wedged port wrote nothing", serial_len == 0);

    /* Eviction: the ring drops whole records, so it never begins mid-line. */
    reset();
    for (i = 0; i < 2000; i++) {
        char *args[1];

        args[0] = WORD((int)i);
        klog_v(KLOG_DEBUG, "fill", "record: %d of two thousand", args);
    }
    CHECK("the ring holds no more than its size", klog_len() <= KLOG_RING_SIZE);
    ring_text(text, sizeof(text));
    CHECK("what it holds begins at a record", text[0] == '<');
    CHECK("and ends at one", text[klog_len() - 1] == '\n');
    CHECK("the oldest records were dropped", !has(text, "record: 0 of"));
    CHECK("the newest was kept", has(text, "record: 1999 of"));
    {
        /* Every line in the ring is whole: a prefix, a subsystem, and a body.
           A ring that dropped bytes rather than records would leave one line
           starting somewhere in the middle of a number. */
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

    /* A record raised from inside the delivery of another, which is what an
       interrupt handler that prints does. Neither may tear the other. */
    reset();
    reenter_from_screen = 1;
    klog(KLOG_INFO, "outer", "began: before the interrupt");
    ring_text(text, sizeof(text));
    CHECK("both records are in the ring", count_lines(text) == 2);
    CHECK("the one raised from inside is whole",
          has(text, "<3>[0.00] irq: landed: in the middle of another record\n"));
    CHECK("and so is the one it interrupted",
          has(text, "<6>[0.00] outer: began: before the interrupt\n"));

    /* The brackets: one build and one ring append per record, balanced. */
    reset();
    klog(KLOG_INFO, "b", "check: the brackets");
    CHECK("a record takes two brackets", irq_pairs == 2);
    CHECK("and leaves none open", irq_depth == 0 && irq_unbalanced == 0);

    if (fails == 0) {
        printf("klog_hosttest: PASS\n");
    }
    return fails;
}
