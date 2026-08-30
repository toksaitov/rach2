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
 * The console calls are the other half of this file. The debug shell's
 * prompts and command output are a user interface, not a diagnostic: they are
 * not the machine's log, and they are not broadcast to whoever is listening on
 * the segment. They share the screen and the port with the records, and that
 * is the one thing the two halves have between them: each console byte says
 * whether it left a line open, so a record never begins in the middle of one.
 */

#include "klog.h"

#include "kio.h"
#include "serial.h"
#include "display/vga_text.h"
#include "lib/kfmt.h"
#include "lib/kirq.h"
#include "network/netconsole.h"

/* The timer's tick counter, at 100 Hz. Read for the timestamp and
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

/**
 * Take one byte of the record being built.
 *
 * A newline inside the text becomes a space, so that one record is one line.
 * Everything downstream depends on that: the ring evicts by finding the next
 * newline, and a record carrying one of its own would be evicted in halves,
 * leaving a tail in the ring with no header on it. The record's own newline
 * is appended past this, after the text is finished.
 *
 * A byte that does not fit is counted and dropped, not written; the caller
 * marks the record as cut.
 */
static void build_putc(char c)
{
    if (c == '\n') {
        c = ' ';
    }
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

/**
 * Write an unsigned decimal.
 *
 * The shared formatter has no conversion for one, and the two numbers in a
 * record's prefix are both unsigned.
 */
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

/**
 * Build `<level>[seconds.hundredths] subsystem: text` and one newline.
 *
 * The prefix is what BusyBox's klogd reads, so a userland log reader can be
 * added later without the kernel changing what it writes. A record that did
 * not fit ends in `...` before its newline rather than simply stopping, so a
 * reader can tell a cut line from a short one.
 *
 * Four bytes are held back from the text: three for that mark and one for the
 * newline every record ends with, whatever the format ended with. Trailing
 * spaces are dropped first, which is where a format written with a newline of
 * its own goes: `build_putc` has already turned it into one.
 */
static void klog_build(struct klog_record *r, int level, const char *subsystem,
                       const char *format, char **args)
{
    uint32_t ticks = kernel_ticks;

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

    while (build_at > r->text && build_at[-1] == ' ') {
        build_at--;
        build_left++;
    }
    build_left += 4;
    if (build_over) {
        build_putc('.');
        build_putc('.');
        build_putc('.');
    }
    /* Written past the sink, which turns a newline into a space: this is the
       one newline a record has, and it is the record's, not the text's. */
    *build_at++ = '\n';
    r->len = (uint32_t)(build_at - r->text);
}

/* ---- destination 1: the ring ---- */

static char ring[KLOG_RING_SIZE];
static uint32_t ring_head;   /* where the next byte goes */
static uint32_t ring_used;   /* bytes held */
static uint32_t ring_oldest; /* where the oldest byte held sits */

/**
 * Drop whole records from the oldest end until `want` bytes are free.
 *
 * The ring holds records and not bytes: a reader handed the tail of a line it
 * never saw the start of would report a subsystem that never spoke. Records
 * carry no newline of their own but the one that ends them, so the next
 * newline is the end of the oldest record.
 */
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

/**
 * Put one record into the ring, dropping as many old ones as it needs.
 *
 * A record longer than the whole ring is refused rather than held in part,
 * which would leave the ring starting mid-line. It cannot happen while the
 * record maximum is far below the ring size; it is refused rather than
 * trusted.
 */
static void ring_append(const char *text, uint32_t n)
{
    uint32_t i;

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
    uint32_t flags = kirq_save();
    uint32_t n = 0;

    if (buf == 0 || cap == 0 || offset >= ring_used) {
        kirq_restore(flags);
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
    kirq_restore(flags);
    return n;
}

/*
 * Whether the console has left a line open on the screen and on the port.
 *
 * A record is a whole line and must begin one: a shell prompt, or a program
 * writing to the terminal without a newline at the end, would otherwise have
 * the next record continue its line, and a reader looking for records at a
 * line start would not find them. The console and the record destinations
 * write to the same screen and the same port, so this is where the two meet.
 *
 * One flag each, because the two destinations do not take the same records: a
 * DEBUG record closes the port's line and never reaches the screen, and a
 * single flag cleared for both would let the next screen record continue the
 * prompt.
 */
static int screen_line_open;
static int serial_line_open;

/* ---- destination 2: the screen ---- */

/**
 * The colour a record is shown in.
 *
 * Debug detail is for the log; a boot narrative is for the screen, because on
 * a machine with no serial port it is the only explanation there is, and the
 * colour is what separates the two lines that matter from the rest.
 */
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

/**
 * Show the record from its body on, without the priority and the timestamp.
 *
 * Those two are for a reader with a capture and a scrollback; a person
 * watching a boot has neither, and every line would begin with the same eight
 * characters of noise.
 */
static void dest_screen(const struct klog_record *r)
{
    uint32_t i;

    if (screen_line_open) {
        k_putc('\n');
        screen_line_open = 0;
    }
    set_fore_color(screen_color(r->level));
    for (i = r->body; i < r->len; i++) {
        k_putc(r->text[i]);
    }
    set_fore_color(VGA_TEXT_DEFAULT_FG);
}

/* ---- destination 3: the serial port ---- */

/**
 * Write the record to the port.
 *
 * The machine's own copy is the ring, which was written before this and which
 * `/dev/bootlog` hands out; there is no second copy to keep here.
 */
static void dest_serial(const struct klog_record *r)
{
    uint32_t i;

    if (serial_line_open) {
        serial_out('\r');
        serial_out('\n');
        serial_line_open = 0;
    }
    for (i = 0; i < r->len; i++) {
        if (r->text[i] == '\n') {
            serial_out('\r');
        }
        serial_out(r->text[i]);
    }
}

/* ---- destination 4: netconsole ---- */

/**
 * Offer the record to netconsole, whether or not it has a device yet.
 *
 * This deliberately does not ask `netconsole_is_enabled`. Netconsole holds
 * what it is given before a device exists and sends it when one arrives, and
 * the records worth having on the wire are exactly the early ones, from a boot
 * that may not reach a shell. Skipping it until enabled would throw those
 * away. A machine that was never asked for netconsole fills that bounded
 * buffer once and drops the rest, which is what it did before.
 */
static void dest_net(const struct klog_record *r)
{
    uint32_t i;

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

/* Set while a record is being written out; see `klog_v`. */
static int klog_delivering;


/**
 * Build one record and offer it to the destinations that take its level.
 *
 * The build runs with interrupts off because the formatter writes through
 * sink state that two callers cannot share. The destinations then run with
 * interrupts as this was entered, so a record raised by an interrupt handler
 * can arrive in the middle of one being written out. Such a record is put in
 * the ring, which is exact either way, and not printed: its bytes inside
 * another record's line, or inside a terminal escape the shell is part way
 * through, are worse than one record being in the log only.
 */
void klog_v(int level, const char *subsystem, const char *format, char **args)
{
    struct klog_record r;
    uint32_t flags;
    int nested, i;

    if (level < KLOG_CRIT) {
        level = KLOG_CRIT;
    }
    if (level > KLOG_DEBUG) {
        level = KLOG_DEBUG;
    }
    flags = kirq_save();
    klog_build(&r, level, subsystem, format, args);
    nested = klog_delivering;
    if (!nested) {
        klog_delivering = 1;
    }
    kirq_restore(flags);

    for (i = 0; i < KLOG_N_DESTS; i++) {
        if (nested && DESTS[i].write != dest_ring) {
            continue;
        }
        if (level <= DESTS[i].takes_down_to) {
            DESTS[i].write(&r);
        }
    }
    if (!nested) {
        klog_delivering = 0;
    }
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    char **args = (char **)&format;

    ++args;
    klog_v(level, subsystem, format, args);
}

/* ---- the console ---- */

/**
 * Write one byte to the screen and the port, expanding the newline.
 *
 * A terminal on the other end of a wire returns to column zero on a carriage
 * return, so a newline written here becomes carriage return and newline on the
 * port. The line discipline has already done that expansion for what it
 * passes on, which is why it writes through `console_putc_raw` instead.
 *
 * The flags are set before the bytes go out, not after: a record raised by an
 * interrupt in the middle of this then knows a line is open and begins its own
 * with a newline. The other order would let it continue the line.
 */
void console_putc(char c)
{
    screen_line_open = c != '\n';
    serial_line_open = c != '\n';
    k_putc(c);
    if (c == '\n') {
        serial_out('\r');
    }
    serial_out(c);
}

/**
 * As `console_putc`, for a writer that has expanded the newline itself.
 *
 * The line discipline expands a newline to carriage return and newline before
 * it reaches here, so this must pass every byte through unchanged; expanding
 * again would put two carriage returns on the wire for every line.
 */
void console_putc_raw(char c)
{
    screen_line_open = c != '\n';
    serial_line_open = c != '\n';
    k_putc(c);
    serial_out(c);
}

/**
 * Write a string and then a newline.
 *
 * The newline is `k_puts`'s, which this stands in for wherever the shell
 * prints a line: a version without it would drop one at every such call, and
 * the difference would show as a scrambled screen rather than as an error.
 * `console_printf`, like `k_printf`, adds nothing.
 */
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
