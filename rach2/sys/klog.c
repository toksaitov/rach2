/*
 * klog.c: one record, four destinations, in an order that survives a failure.
 *
 * Everything the kernel says about itself is built here into one whole record
 * and then offered to the ring, the screen, the serial port and netconsole, in
 * that order. The order is the point. The ring is the machine's own copy and is
 * written first, so a serial port that stopped draining or an Ethernet
 * controller that never came up cannot take the log with it; the destinations
 * that can fail come after the one that cannot, and each of them is bounded.
 *
 * A record is formatted whole before any destination sees it. Two things follow
 * from that: a reader never meets half a line, and a record raised by an
 * interrupt handler cannot appear inside one raised by the code it interrupted.
 * The formatter writes through a sink with state of its own, which is the one
 * thing here that two callers cannot share, so the formatting and the ring
 * append each run with interrupts off. Both are short and neither sleeps.
 *
 * The console calls are the other half of this file and deliberately do not
 * touch any of the above. The debug shell's prompts and command output are a
 * user interface, not a diagnostic: they are not the machine's log, and they
 * are not broadcast to whoever is listening on the segment.
 */

#include "klog.h"

#include "kio.h"
#include "serial.h"
#include "display/vga_text.h"
#include "lib/kfmt.h"
#include "lib/kirq.h"
#include "network/netconsole.h"

/* The tick counter the scheduler keeps, at 100 Hz. Read for the timestamp and
   for nothing else; before the timer starts it is zero and a record reads
   `[0.00]`, which is the truth about a machine that has no clock yet. */
extern volatile uint32_t kernel_ticks;

/* ---- the record ---- */

struct klog_record {
    char text[KLOG_RECORD_MAX];
    uint32_t len;   /* bytes in `text`, including the newline */
    uint32_t body;  /* where `subsystem: ` starts, past the prefix */
    int level;
};

/* Where the formatter's sink is writing. It is file-static because
   `k_vprintf` takes a function and not a context, and it is why the build
   below runs with interrupts off. */
static char *build_at;
static uint32_t build_left;
static uint32_t build_over;

static void build_putc(char c)
{
    if (build_left > 0) {
        *build_at++ = c;
        build_left--;
    } else {
        build_over = 1;
    }
}

static void build_str(const char *s)
{
    while (s != 0 && *s != '\0') {
        build_putc(*s++);
    }
}

/* An unsigned decimal, which the shared formatter has no conversion for and
   which the two numbers in the prefix both are. */
static void build_u32(uint32_t v)
{
    char digits[10];
    int n = 0;

    if (v == 0) {
        build_putc('0');
        return;
    }
    while (v > 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        build_putc(digits[--n]);
    }
}

/*
 * Build `<level>[seconds.hundredths] subsystem: text` and one newline.
 *
 * The prefix is what BusyBox's klogd reads, so a userland log reader can be
 * added later without the kernel changing what it writes. A record that did
 * not fit ends in `...` before its newline rather than simply stopping, so a
 * reader can tell a cut line from a short one.
 */
static void klog_build(struct klog_record *r, int level, const char *subsystem,
                       const char *format, char **args)
{
    uint32_t ticks = kernel_ticks;

    /* One byte is held back for the newline, which every record has whatever
       the format said, and three more for the truncation mark. */
    build_at = r->text;
    build_left = KLOG_RECORD_MAX - 4;
    build_over = 0;

    r->level = level;
    build_putc('<');
    build_u32((uint32_t)level);
    build_putc('>');
    build_putc('[');
    build_u32(ticks / 100u);
    build_putc('.');
    build_putc((char)('0' + (ticks / 10u) % 10u));
    build_putc((char)('0' + ticks % 10u));
    build_putc(']');
    build_putc(' ');
    r->body = (uint32_t)(build_at - r->text);

    build_str(subsystem);
    build_putc(':');
    build_putc(' ');
    k_vprintf(build_putc, format, args);

    /* Whatever the format ended with, the record ends with exactly one
       newline: a destination that framed records by the newline would
       otherwise join two of them. */
    while (build_at > r->text && build_at[-1] == '\n') {
        build_at--;
        build_left++;
    }
    build_left += 4;
    if (build_over) {
        build_putc('.');
        build_putc('.');
        build_putc('.');
    }
    build_putc('\n');
    r->len = (uint32_t)(build_at - r->text);
}

/* ---- destination 1: the ring ---- */

static char ring[KLOG_RING_SIZE];
static uint32_t ring_head;   /* where the next byte goes */
static uint32_t ring_used;   /* bytes held */
static uint32_t ring_oldest; /* where the oldest byte held sits */

/* Drop whole records from the oldest end until `want` bytes are free. The
   ring holds records and not bytes: a reader that was handed the tail of a
   line it never saw the start of would report a subsystem that never spoke. */
static void ring_evict(uint32_t want)
{
    while (KLOG_RING_SIZE - ring_used < want && ring_used > 0) {
        uint32_t n = 0;

        while (n < ring_used) {
            uint32_t i = ring_oldest + n;

            if (i >= KLOG_RING_SIZE) {
                i -= KLOG_RING_SIZE;
            }
            n++;
            if (ring[i] == '\n') {
                break;
            }
        }
        ring_oldest += n;
        if (ring_oldest >= KLOG_RING_SIZE) {
            ring_oldest -= KLOG_RING_SIZE;
        }
        ring_used -= n;
    }
}

static void ring_append(const char *text, uint32_t n)
{
    uint32_t i;

    /* A record longer than the whole ring cannot be held and is not held in
       part: the ring would then start mid-line, which is the one thing it
       promises not to do. It cannot happen while the record maximum is far
       below the ring size, and it is refused rather than trusted. */
    if (n > KLOG_RING_SIZE) {
        return;
    }
    ring_evict(n);
    for (i = 0; i < n; i++) {
        ring[ring_head] = text[i];
        ring_head++;
        if (ring_head == KLOG_RING_SIZE) {
            ring_head = 0;
        }
    }
    ring_used += n;
}

static void dest_ring(const struct klog_record *r)
{
    uint32_t flags = kirq_save();

    ring_append(r->text, r->len);
    kirq_restore(flags);
}

uint32_t klog_len(void)
{
    return ring_used;
}

uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset)
{
    uint32_t n = 0;

    if (buf == 0 || cap == 0 || offset >= ring_used) {
        return 0;
    }
    while (n < cap && offset + n < ring_used) {
        uint32_t i = ring_oldest + offset + n;

        while (i >= KLOG_RING_SIZE) {
            i -= KLOG_RING_SIZE;
        }
        buf[n] = ring[i];
        n++;
    }
    return n;
}

/* ---- destination 2: the screen ---- */

/* The level a person in front of the machine is shown, and the colour it is
   shown in. Debug detail is for the log; a boot narrative is for the screen,
   because on a machine with no serial port it is the only explanation there
   is. */
static display_color_t screen_color(int level)
{
    if (level <= KLOG_ERR) {
        return light_red;
    }
    if (level == KLOG_WARN) {
        return brown;
    }
    return VGA_TEXT_DEFAULT_FG;
}

static void dest_screen(const struct klog_record *r)
{
    uint32_t i;

    set_fore_color(screen_color(r->level));
    /* From `body`: the priority and the timestamp are for a reader with a
       capture and a scrollback, and a person watching a boot has neither. */
    for (i = r->body; i < r->len; i++) {
        k_putc(r->text[i]);
    }
    set_fore_color(VGA_TEXT_DEFAULT_FG);
}

/* ---- destination 3: the serial port ---- */

static void dest_serial(const struct klog_record *r)
{
    uint32_t i;

    for (i = 0; i < r->len; i++) {
        if (r->text[i] == '\n') {
            serial_out('\r');
        }
        serial_out(r->text[i]);
    }
}

/* ---- destination 4: netconsole ---- */

static void dest_net(const struct klog_record *r)
{
    uint32_t i;

    if (!netconsole_is_enabled()) {
        return;
    }
    for (i = 0; i < r->len; i++) {
        netconsole_putc(r->text[i]);
    }
}

/* ---- the table ---- */

/*
 * The destinations, in the order a record reaches them, each with the least
 * urgent level it accepts. A table and not a registry of subscribers: the four
 * are known at build time, nothing may be added while the machine runs, and an
 * order that is data is an order a reader can check.
 */
struct klog_dest {
    int takes_down_to;
    void (*write)(const struct klog_record *r);
};

static const struct klog_dest DESTS[] = {
    { KLOG_DEBUG, dest_ring },
    { KLOG_INFO,  dest_screen },
    { KLOG_DEBUG, dest_serial },
    { KLOG_DEBUG, dest_net }
};

#define KLOG_N_DESTS ((int)(sizeof(DESTS) / sizeof(DESTS[0])))

void klog_v(int level, const char *subsystem, const char *format, char **args)
{
    struct klog_record r;
    uint32_t flags;
    int i;

    if (level < KLOG_CRIT) {
        level = KLOG_CRIT;
    }
    if (level > KLOG_DEBUG) {
        level = KLOG_DEBUG;
    }
    /* The build, and only the build: the sink's state is the one thing a
       second caller would corrupt, and the destinations below take a record
       that is already whole. */
    flags = kirq_save();
    klog_build(&r, level, subsystem, format, args);
    kirq_restore(flags);

    for (i = 0; i < KLOG_N_DESTS; i++) {
        if (level <= DESTS[i].takes_down_to) {
            DESTS[i].write(&r);
        }
    }
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    char **args = (char **)&format;

    ++args;
    klog_v(level, subsystem, format, args);
}

/* ---- the console ---- */

/*
 * The screen and the serial port, written byte by byte as they are given.
 *
 * This is the shell's terminal, so the bytes are passed through unchanged:
 * an escape sequence a line editor sent has to arrive as the sequence. The
 * newline is expanded to carriage return and newline for the serial side
 * alone, which is what a terminal on the other end of a wire expects.
 */
void console_putc(char c)
{
    k_putc(c);
    if (c == '\n') {
        serial_out('\r');
    }
    serial_out(c);
}

/* The newline at the end is `k_puts`'s, which this stands in for wherever the
   shell prints a line; a version without it would drop a newline at every one
   of those calls and the difference would show as a scrambled screen rather
   than as an error. `console_printf`, like `k_printf`, adds nothing. */
void console_puts(const char *str)
{
    if (str == 0) {
        return;
    }
    while (*str != '\0') {
        console_putc(*str++);
    }
    console_putc('\n');
}

void console_printf(const char *format, ...)
{
    char **args = (char **)&format;

    ++args;
    k_vprintf(console_putc, format, args);
}
