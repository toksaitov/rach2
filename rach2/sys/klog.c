/*
 * klog.c: the kernel log, each record built whole and offered to four
 * destinations, the ring first; and the debug shell's console calls.
 *
 * Everything the kernel says about itself is built here into one whole record
 * and then offered to the ring, the screen, the serial port and netconsole, in
 * that order. The ring is the machine's own copy and is written first, so a
 * serial port that stopped draining or an Ethernet controller that never came
 * up cannot take the log with it; the destinations that can fail come after
 * the one that cannot, and each of them is bounded.
 *
 * A record is formatted whole before any destination sees it. Two things follow
 * from that: a reader never meets half a line, and a record raised by an
 * interrupt handler cannot appear inside one raised by the code it
 * interrupted. It waits in a queue instead, and the delivery it interrupted
 * writes it out when its own record is finished.
 *
 * The formatter writes through a sink with state of its own, one for each
 * processor, which two callers on one processor cannot share, so the
 * formatting and the ring append each run with interrupts off. Both are
 * short and neither sleeps.
 *
 * The ring, the queue and the destinations are shared by every processor,
 * and the big lock is what keeps two of them out at once. A record made
 * where that does not hold, in an NMI or by a processor without the big
 * lock, as the lockup detectors' are, is kept whole in a queue of its own
 * that takes records from any processor without a lock, and a processor
 * that may write the rest moves it on at its next record, console write or
 * tick, or at the panic (`klog_flush`), as Linux's printk defers the legacy
 * consoles in an NMI. When the holder of the big lock is locked up, another
 * processor writes them out instead (`klog_flush_stuck`), and the two keep
 * out of each other's way (`klog_enter`).
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
#include "arch/arch.h"
#include "display/textcon.h"
#include "lib/kfmt.h"
#include "lib/kirq.h"
#include "lib/kdef.h"
#include "network/netconsole.h"
#include "panic.h"
#include "proc/percpu.h"
#include "proc/smp.h"
#include "pstore/pstore.h"
#include "timer.h"
#include "watchdog.h"

/* The microseconds in one tick of `kernel_ticks`, which is read for the
   timestamp without the tick's sequence count, since a record may be made
   where waiting for it is not allowed; before the timer starts it is zero,
   and a record reads `[0.000000]`. */
#define KLOG_TICK_US TICK_US

/* ---- the record ---- */

struct klog_record {
    char text[KLOG_RECORD_MAX];
    uint32_t len;  /* bytes in `text`, including the newline */
    uint32_t body; /* where `subsystem: ` starts, past the prefix */
    int level;
    /* In the queue of records waiting for the destinations past the ring,
     * the sequence number, plus one, of the kept record it was moved from
     * (`defer_collect`); 0 for any other. */
    uint32_t kept;
};

/* Where each processor's formatter is writing: the next byte, the room
   left and whether a byte did not fit. It is file-static because
   `k_vprintf` takes a function and not a context, and it is why the build
   below runs with interrupts off. */
static struct {
    char *at;
    uint32_t left;
    uint32_t over;
} sinks[NR_CPUS];

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
    unsigned int me = arch_cpu_id();

    if (c == '\n') {
        c = ' ';
    }
    if (sinks[me].left > 0) {
        *sinks[me].at++ = c;
        sinks[me].left--;
    } else {
        sinks[me].over = 1;
    }
}

/** Take the string `s` into the record; NULL takes nothing. */
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
 * Build `<level>[seconds.microseconds] subsystem: text` and one newline.
 *
 * The prefix is the form BusyBox's klogd reads, and the stamp Linux's, with
 * six digits after the point, which util-linux's dmesg reads as
 * microseconds; it moves by whole ticks. The seconds are not padded, so the
 * stamp is digits, a point and digits. A record that did not fit
 * ends in `...` before its newline rather than simply stopping, so a reader
 * can tell a cut line from a short one.
 *
 * Four bytes are held back from the text: three for that mark and one for the
 * newline every record ends with, whatever the format ended with. Trailing
 * spaces are dropped first, which is where a format written with a newline of
 * its own goes: `build_putc` has already turned it into one. The record's
 * newline is written past the sink, which would turn it into a space too.
 *
 * The sink's place is put back as it was at the end, since an NMI that logs,
 * as the watchdog's does, can come in the middle of another record's build
 * on the same processor, whose interrupts are off.
 */
static void klog_build(struct klog_record *r, int level, const char *subsystem, const char *format,
                       va_list ap)
{
    unsigned int me = arch_cpu_id();
    uint32_t rest;
    uint32_t seconds = (uint32_t)tick_div(kernel_ticks, TICK_HZ, &rest);
    uint32_t us = rest * KLOG_TICK_US;
    uint32_t place;
    char *was_at = sinks[me].at;
    uint32_t was_left = sinks[me].left, was_over = sinks[me].over;

    sinks[me].at = r->text;
    sinks[me].left = KLOG_RECORD_MAX - 4;
    sinks[me].over = 0;

    r->level = level;
    r->kept = 0;
    build_putc('<');
    build_u32((uint32_t)level);
    build_putc('>');
    build_putc('[');
    build_u32(seconds);
    build_putc('.');
    for (place = 100000u; place > 0; place /= 10u) {
        build_putc((char)('0' + us / place % 10u));
    }
    build_putc(']');
    build_putc(' ');
    r->body = (uint32_t)(sinks[me].at - r->text);

    build_str(subsystem);
    build_putc(':');
    build_putc(' ');
    k_vprintf(build_putc, format, ap);

    while (sinks[me].at > r->text && sinks[me].at[-1] == ' ') {
        sinks[me].at--;
        sinks[me].left++;
    }
    sinks[me].left += 4;
    if (sinks[me].over) {
        build_putc('.');
        build_putc('.');
        build_putc('.');
    }
    *sinks[me].at++ = '\n';
    r->len = (uint32_t)(sinks[me].at - r->text);
    sinks[me].at = was_at;
    sinks[me].left = was_left;
    sinks[me].over = was_over;
}

/** Where the timestamp of `r` begins, past its `<level>`, which its body
 * follows; `r->len`, past every byte, when the record has none. */
static uint32_t record_stamp(const struct klog_record *r)
{
    uint32_t i;

    for (i = 0; i + 1u < r->body; i++) {
        if (r->text[i] == '>') {
            return r->text[i + 1u] == '[' ? i + 1u : r->len;
        }
    }
    return r->len;
}

/* ---- destination 1: the ring ---- */

static char ring[KLOG_RING_SIZE];

/* Where the ring's records are. */
struct ring_state {
    uint32_t head;   /* where the next byte goes */
    uint32_t used;   /* bytes held */
    uint32_t oldest; /* where the oldest byte held is */
    /* Bytes ever put in the ring: a reader that keeps its place counts from
     * the first, and the oldest byte held is this less `used`. */
    uint32_t written;
    /* The sequence number, plus one, of the last kept record put in
     * (`defer_collect`), or 0. */
    uint32_t kept;
};

/* The ring's state, two copies: `ring_cur` names the one that holds, and an
 * append fills the other and then names it, one store, so that a panic
 * which stops the append at any instruction finds the state from before it
 * or after it, with the bytes it names whole, as Linux's ring buffer gives
 * a reader only the records a writer has committed (desc_reserved and
 * desc_committed, kernel/printk/printk_ringbuffer.h:116-120). */
static struct ring_state ring_st[2];
static volatile unsigned int ring_cur;

/* Held, with interrupts off, by a processor that reads or writes the ring:
 * the one that writes the kept records out for a locked-up holder of the
 * big lock (`klog_flush_stuck`) appends while the holder, should it come
 * back, may read. */
static volatile unsigned int ring_lock;

/**
 * Take `ring_lock`, with interrupts off. 1 when taken; 0 when a panic on
 * this processor found it held, by a processor the panic has stopped,
 * which will never let it go, and goes on without it: an append that one
 * left part done is not in the state `ring_cur` names (`ring_append`).
 */
static int ring_get(void)
{
    while (arch_atomic_xchg(&ring_lock, 1u) != 0u) {
        if (k_panicking()) {
            return 0;
        }
        arch_relax();
    }
    return 1;
}

/** Let `ring_lock` go when `ring_get` returned 1 in `took`. */
static void ring_put(int took)
{
    if (took) {
        k_barrier();
        ring_lock = 0;
    }
}

/**
 * Drop whole records from the oldest end until `want` bytes are free.
 *
 * The ring holds records and not bytes: the tail of a line, without its
 * prefix and subsystem, would read as a record of the wrong subsystem or of
 * none. A record has no newline but the one that ends it, so the next
 * newline is the end of the oldest record.
 */
static void ring_evict(struct ring_state *s, uint32_t want)
{
    while (KLOG_RING_SIZE - s->used < want && s->used > 0) {
        uint32_t n = 0;

        while (n < s->used) {
            uint32_t i = s->oldest + n;

            if (i >= KLOG_RING_SIZE) {
                i -= KLOG_RING_SIZE;
            }
            n++;
            if (ring[i] == '\n') {
                break;
            }
        }
        s->oldest += n;
        if (s->oldest >= KLOG_RING_SIZE) {
            s->oldest -= KLOG_RING_SIZE;
        }
        s->used -= n;
    }
}

/** Fill the copy of the ring's state that does not hold from the one that
 * does, and return it. */
static struct ring_state *ring_next(void)
{
    struct ring_state *s = &ring_st[ring_cur ^ 1u];

    *s = ring_st[ring_cur];
    return s;
}

/** Name the copy `ring_next` filled as the one that holds, after its
 * stores and those of the bytes it names. */
static void ring_commit(void)
{
    k_barrier();
    ring_cur ^= 1u;
    k_barrier();
}

/**
 * Put one record into the ring, dropping as many old ones as it needs, and
 * note `kept`, when not 0, as the last kept record put in.
 *
 * A record longer than the whole ring is dropped rather than held in part,
 * which would leave the ring starting mid-line. It cannot happen while the
 * record maximum is far below the ring size; the check keeps the ring whole
 * if a change to either size ever lets it.
 *
 * The eviction is committed before a byte is written over what it freed,
 * and the record after its last byte, so that the state that holds names
 * whole records at every instruction.
 */
static void ring_append(const char *text, uint32_t n, uint32_t kept)
{
    struct ring_state *s;
    uint32_t i, at;
    int took;

    if (n > KLOG_RING_SIZE) {
        return;
    }
    took = ring_get();
    s = ring_next();
    ring_evict(s, n);
    ring_commit();
    at = s->head;
    for (i = 0; i < n; i++) {
        ring[at] = text[i];
        at++;
        if (at == KLOG_RING_SIZE) {
            at = 0;
        }
    }
    s = ring_next();
    s->head = at;
    s->used += n;
    s->written += n;
    if (kept != 0u) {
        s->kept = kept;
    }
    ring_commit();
    ring_put(took);
}

/** Put the record in the ring and in ramoops's console zone, with
 * interrupts off. */
static void dest_ring(const struct klog_record *r)
{
    uint32_t flags = kirq_save();

    ring_append(r->text, r->len, 0u);
    pstore_console_write(r->text, r->len);
    kirq_restore(flags);
}

uint32_t klog_len(void)
{
    return ring_st[ring_cur].used;
}

uint32_t klog_written(void)
{
    return ring_st[ring_cur].written;
}

uint32_t klog_clamp(uint32_t at)
{
    uint32_t flags = kirq_save();
    int took = ring_get();
    const struct ring_state *s = &ring_st[ring_cur];
    uint32_t first = s->written - s->used;

    if ((int32_t)(at - first) < 0 || (int32_t)(s->written - at) < 0) {
        at = first;
    }
    ring_put(took);
    kirq_restore(flags);
    return at;
}

uint32_t klog_read_from(char *buf, uint32_t cap, uint32_t *at)
{
    uint32_t flags = kirq_save();
    int took = ring_get();
    const struct ring_state *s = &ring_st[ring_cur];
    uint32_t first = s->written - s->used;
    uint32_t n = 0;

    if ((int32_t)(*at - first) < 0 || (int32_t)(s->written - *at) < 0) {
        *at = first;
    }
    while (n < cap && *at + n != s->written) {
        uint32_t i = s->oldest + (*at - first) + n;

        if (i >= KLOG_RING_SIZE) {
            i -= KLOG_RING_SIZE;
        }
        buf[n++] = ring[i];
    }
    *at += n;
    ring_put(took);
    kirq_restore(flags);
    return n;
}

uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset)
{
    uint32_t flags = kirq_save();
    int took = ring_get();
    const struct ring_state *s = &ring_st[ring_cur];
    uint32_t n = 0;

    if (buf == 0 || cap == 0 || offset >= s->used) {
        ring_put(took);
        kirq_restore(flags);
        return 0;
    }
    while (n < cap && offset + n < s->used) {
        uint32_t i = s->oldest + offset + n;

        while (i >= KLOG_RING_SIZE) {
            i -= KLOG_RING_SIZE;
        }
        buf[n] = ring[i];
        n++;
    }
    ring_put(took);
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
 * One flag each, this one and `serial_line_open` below, because the two
 * destinations do not take the same records: a DEBUG record closes the port's
 * line and never reaches the screen, and a single flag cleared for both would
 * let the next screen record continue the prompt.
 */
/* Set when the screen gave up on the console's lock, which the next record
 * then tries once, without the wait. */
static int screen_contended;

static int screen_line_open;

/*
 * The lowest-priority record the screen shows.
 *
 * The screen is not the log. The ring keeps every record whatever this is set
 * to, and so do the serial port and netconsole, so nothing is lost by turning
 * the screen down; what changes is only what a person standing at the machine
 * is made to read.
 *
 * The default keeps the boot log, which is `KLOG_INFO`, because a screen that
 * says nothing while a machine starts cannot be told from a machine that has
 * stopped. What it drops is `KLOG_DEBUG`. A program that is talking to a
 * person turns it down further for its own run and puts it back: that is what
 * `rach2-install` does, since a routine note arriving in the middle of a
 * question reads as something having gone wrong. `KLOG_CRIT`, which a fault
 * that panics raises, no setting can hide; the panic itself stops the
 * screen's records (`screen_stopped`) and draws its own report.
 */
static int screen_level = KLOG_INFO;

/* printk's `ignore_loglevel`: the screen shows every record, KLOG_DEBUG
 * among them, whatever `screen_level` says (`klog_set_ignore_loglevel`). */
static volatile int screen_all_levels;

/* printk's `time`: the serial port's and plain netconsole's lines carry
 * the record's timestamp (`klog_set_time`). */
static volatile int show_time = 1;

/* Whether a panic has stopped the screen's records (`klog_screen_stop`). */
static int screen_stopped;

/* Whether the boot line named a level. A boot brought up with
   `rach2.loglevel=` asked to watch the machine, and the handover to init
   leaves that alone rather than taking the screen back off the person who
   asked for it. */
static int screen_level_asked_for;

/* Whether the console has left a line open on the port; see
 * `screen_line_open`. */
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
    return DEFAULT_FORECOLOR;
}

/**
 * Show the record from its body on, without the priority and the timestamp,
 * when its level is `screen_level` or more urgent and not KLOG_DEBUG, or
 * whatever its level under printk's `ignore_loglevel`, until a panic stops
 * the screen's records.
 *
 * Those two are for a reader with a capture and a scrollback; a person
 * watching a boot has neither, and every line would begin with the same eight
 * characters of noise. A record whose console lock another processor has
 * held for a second is left off the screen (`console_lock_bounded`), since
 * that processor may be the one locked up, and so is each after it while
 * the lock is still held, without the wait.
 */
static void dest_screen(const struct klog_record *r)
{
    uint32_t i, flags;

    if (screen_stopped ||
        (!screen_all_levels && (r->level > screen_level || r->level > KLOG_INFO))) {
        return;
    }
    if (!console_lock_bounded(&flags, screen_contended ? 0u : 1000000u)) {
        screen_contended = 1;
        return; /* the holder is stuck; the ring and the port have it */
    }
    screen_contended = 0;
    if (screen_line_open) {
        k_putc('\n');
        screen_line_open = 0;
    }
    set_fore_color(screen_color(r->level));
    textcon_hold();
    for (i = r->body; i < r->len; i++) {
        k_putc(r->text[i]);
    }
    textcon_release();
    set_fore_color(DEFAULT_FORECOLOR);
    console_unlock(flags);
}

/* ---- destination 3: the serial port ---- */

/*
 * klog's report lock: the number, plus one, of the processor that writes a
 * record to the port, or 0. A holder of the big lock writes the port under
 * it as a processor without it writes a report (`klog_report`), so the
 * port has each record whole. It is the lock-order table's last row with
 * the checker's own (lib/lockdep.Readme.md): nothing is taken under it.
 */
static volatile unsigned int report_owner;

/* KLOG_REPORT_WAIT_MS in cycles of the time-stamp counter, counted at the
 * serial port's rate, faster than any processor's counter runs. */
#define KLOG_REPORT_WAIT_CYCLES (KLOG_REPORT_WAIT_MS * 5000000UL)

/**
 * Take the report lock: KLOG_REPORT_LOCKED when taken; KLOG_REPORT_RING
 * when this processor holds it, as an interrupt of the holder finds it, or
 * when `bounded` and another held it for KLOG_REPORT_WAIT_MS;
 * KLOG_REPORT_PANIC when a panic finds it held by a processor the panic
 * has stopped. Only this processor writes its own number, so the first
 * reading cannot be stale for it.
 */
static int report_get(int bounded)
{
    unsigned int me = (unsigned int)arch_cpu_id() + 1u;
    uint64_t start;

    if (report_owner == me) {
        return KLOG_REPORT_RING;
    }
    start = arch_cycles();
    while (arch_atomic_cmpxchg(&report_owner, 0u, me) != 0u) {
        if (k_panicking()) {
            return KLOG_REPORT_PANIC;
        }
        if (bounded && arch_cycles() - start >= (uint64_t)KLOG_REPORT_WAIT_CYCLES) {
            return KLOG_REPORT_RING;
        }
        arch_relax();
    }
    return KLOG_REPORT_LOCKED;
}

/** Let the report lock go when `report_get` gave `how`. */
static void report_put(int how)
{
    if (how == KLOG_REPORT_LOCKED) {
        k_barrier();
        report_owner = 0;
    }
}

/**
 * Write the record `r` to the port, after a newline that ends a line the
 * console left open, with each newline as a carriage return and a newline,
 * and without its timestamp while `klog_time` is off, as Linux's console
 * line leaves out `[seconds.microseconds]` without `printk.time`
 * (`info_print_prefix`, kernel/printk/printk.c). The caller holds the
 * report lock, or a panic went on without it.
 */
static void serial_record(const struct klog_record *r)
{
    uint32_t i, stamp = show_time ? r->len : record_stamp(r);

    if (serial_line_open) {
        serial_out('\r');
        serial_out('\n');
        serial_line_open = 0;
    }
    for (i = 0; i < r->len; i++) {
        if (i == stamp) {
            i = r->body - 1u;
            continue;
        }
        if (r->text[i] == '\n') {
            serial_out('\r');
        }
        serial_out(r->text[i]);
    }
}

/**
 * Write the record to the port.
 *
 * The machine's own copy is the ring, which was written before this and which
 * `/dev/bootlog` hands out; there is no second copy to keep here.
 */
static void dest_serial(const struct klog_record *r)
{
    int how = report_get(0);

    serial_record(r);
    report_put(how);
}

/* ---- destination 4: netconsole ---- */

/*
 * Netconsole's share of the records that a processor without the big lock
 * writes out for a holder that is locked up (`klog_flush_stuck`), whole
 * records in the order written, which the next processor that may write
 * klog's state sends ahead of anything of its own (`net_send_owed`).
 * Netconsole, the Ethernet frame buffer and each driver's transmit ring
 * have no guard but the big lock (network/net.h), and the holder that comes
 * back runs them while that writer is still at work, so the writer leaves
 * them alone. Linux's writer likewise leaves a console it cannot lock to
 * the lock's holder, which writes each console from that console's own
 * place in the ring when it unlocks (`console_trylock`,
 * kernel/printk/printk.c:4511-4512; `con->seq`, :3081-3086), and
 * netconsole's send only trylocks the device's queue
 * (`HARD_TX_TRYLOCK`, net/core/netpoll.c:340-374). The room holds the
 * hundred or so records of a hard-lockup report with backtraces; a record
 * that does not fit is counted (`net_owed_lost`) and reported once the
 * rest are sent.
 */
#define KLOG_NET_OWED 16384

static char net_owed[KLOG_NET_OWED];
static uint32_t net_owed_len;  /* bytes held */
static uint32_t net_owed_sent; /* bytes of them netconsole has had */
static int net_owed_lost;      /* records that did not fit */

/* The records of `net_owed_lost` that a send of the held records found,
 * which the drain reports (`klog_drain`). */
static int net_owed_dropped;

/* 1 while `klog_flush_stuck` writes the records out; only one processor
 * writes klog's state at a time (`klog_enter`), so it is that one's. */
static int net_stuck;

/**
 * Give netconsole the records held for it, from where a send that a panic
 * stopped left off. The caller may write klog's state and is not
 * `klog_flush_stuck`.
 */
static void net_send_owed(void)
{
    uint32_t i;

    for (i = net_owed_sent; i < net_owed_len; i++) {
        netconsole_putc(net_owed[i]);
        net_owed_sent = i + 1u;
    }
    net_owed_dropped += net_owed_lost;
    net_owed_lost = 0;
    net_owed_len = 0;
    net_owed_sent = 0;
}

/**
 * Offer the record to netconsole, whether or not it has a device yet, after
 * the records held for it; from `klog_flush_stuck`, hold it instead.
 *
 * This deliberately does not ask `netconsole_is_enabled`. Netconsole holds
 * what it is given before a device exists and sends it when one arrives, and
 * the records worth having on the wire are exactly the early ones, from a boot
 * that may not reach a shell. Skipping it until enabled would throw those
 * away. A machine that was never asked for netconsole fills that bounded
 * buffer once and drops the rest.
 */
static void dest_net(const struct klog_record *r)
{
    uint32_t i;

    if (net_stuck) {
        if (r->len > KLOG_NET_OWED - net_owed_len) {
            net_owed_lost++;
            return;
        }
        for (i = 0; i < r->len; i++) {
            net_owed[net_owed_len + i] = r->text[i];
        }
        net_owed_len += r->len;
        return;
    }
    if (net_owed_len != 0) {
        net_send_owed();
    }
    for (i = 0; i < r->len; i++) {
        netconsole_putc(r->text[i]);
    }
}

/* ---- the table ---- */

/*
 * The destinations, in the order a record reaches them; each takes every
 * level but the screen, which decides for itself (`dest_screen`). A table
 * and not a registry of subscribers: the four are known at build time,
 * nothing may be added while the machine runs, and an order that is data is
 * an order a reader can check.
 */
struct klog_dest {
    void (*write)(const struct klog_record *r);
};

static const struct klog_dest DESTS[] = {{dest_ring}, {dest_screen}, {dest_serial}, {dest_net}};

#define KLOG_N_DESTS ((int)(sizeof(DESTS) / sizeof(DESTS[0])))

/* Set while a record is being written out; see `klog_v`. */
static int klog_delivering;

/* How many console writes are in progress; see `console_write_begin`. A
   record raised while one is waits in the queue as one raised inside
   another record's delivery does. */
static int console_writing;

/*
 * The records raised while another was being written out, waiting for the
 * destinations past the ring.
 *
 * A record from an interrupt handler cannot go out where it is raised: its
 * bytes would land inside the line the interrupted code is part way through.
 * It waits here and goes out when that line is finished, so what a machine
 * says on its port is what its ring has, an event that happens while the
 * machine is printing included.
 *
 * The largest burst is a USB hub pulled with the device table full behind
 * it: one pass of the USB stack, which runs in an interrupt, says each of
 * sixteen devices is gone and gives back its slot, and a disk says more.
 * Sixty-four records hold four for each, beside the record a drain is
 * writing out, which keeps its slot until every destination has had it.
 * One more is counted and dropped, and the count is reported once the queue
 * is empty, because a log that quietly loses records is worse than one that
 * says how many.
 */
#define KLOG_PENDING_MAX 64

/* The queue's slots: room for `KLOG_PENDING_MAX` records and the one in
 * flight, rounded up to a power of two so that it divides 2^32. */
#define KLOG_PENDING_SLOTS 128

/* The queue's records are those numbered from `pending_first` up to
 * `pending_next`, each in the slot of its number modulo
 * `KLOG_PENDING_SLOTS`: putting one moves only `pending_next`, and taking
 * one only `pending_first`, each in one store, so that a panic which stops
 * either at any instruction finds the queue whole. */
static struct klog_record pending[KLOG_PENDING_SLOTS];
static unsigned int pending_first;
static unsigned int pending_next;
static int pending_lost;

/* 1 while `klog_drain` writes the queue's first record out, which then does
 * not count against `KLOG_PENDING_MAX`. */
static int pending_writing;

/* Which destinations have had a record of the queue whole: its number times
 * sixteen, plus 1 << i for each `DESTS[i]` that has, so that one store
 * names both the record and the destinations and a panic that stops a drain
 * finds them agreeing. The four destinations fit the four bits. */
static unsigned int pending_had;

/** How many records the queue holds, the one in flight included. */
static int pending_count(void)
{
    return (int)(pending_next - pending_first);
}

/** Whether the queue holds as many records as it takes, besides the one a
 * drain is writing out. */
static int pending_full(void)
{
    return pending_count() - pending_writing >= KLOG_PENDING_MAX;
}

/**
 * The destinations that have had the queue's record numbered `n` whole, as
 * `pending_had` gives them, 1 << i for each `DESTS[i]`; 0 when
 * `pending_had` names another record.
 */
static unsigned int pending_had_by(unsigned int n)
{
    unsigned int had = pending_had;

    return (had >> 4) == (n & 0x0fffffffu) ? had & 0xfu : 0u;
}

/** The newest record in the queue, which holds one. */
static const struct klog_record *pending_last(void)
{
    return &pending[(pending_next - 1u) % KLOG_PENDING_SLOTS];
}

/**
 * Put a record at the end of the queue, or count it as lost; `kept` is the
 * sequence number, plus one, of the kept record it is, or 0.
 *
 * The caller has interrupts off: this runs inside the same region that
 * built the record and read `klog_delivering`. The end moves after the
 * copy, so a panic that stops it finds the record queued whole or not.
 */
static void pending_put(const struct klog_record *r, uint32_t kept)
{
    struct klog_record *q;

    if (pending_full()) {
        pending_lost++;
        return;
    }
    q = &pending[pending_next % KLOG_PENDING_SLOTS];
    *q = *r;
    q->kept = kept;
    k_barrier();
    pending_next++;
}

/**
 * Write the queued records to the destinations past the ring, in the order
 * they were raised, and then let the next record be delivered directly.
 *
 * `klog_delivering` stays set for the whole drain, so a record raised by an
 * interrupt during it joins the queue instead of the line being written. The
 * flag is cleared with the queue empty and interrupts off, which leaves no
 * moment where a record is waiting and nobody is coming for it.
 *
 * A record leaves the queue only once every destination has had it, and
 * each destination that has is marked in `pending_had`, so a panic that
 * stops the drain part way through one finds it still first in the queue
 * and writes it out again whole to the destination that was writing it and
 * to those after, and not to those before. So Linux keeps a sequence
 * number for each console and moves it past a record only after that
 * console's write (`con->seq`, `console_emit_next_record`,
 * kernel/printk/printk.c:3081-3086); it walks each console from its own
 * number (`console_flush_one_record`, :3144-3186), and its panic writes
 * from there on (`console_flush_on_panic`, :3449-3482), so only the
 * console that was writing has the record again. Records held for
 * netconsole while a holder was locked up (`net_owed`) go to it first.
 */
static void klog_drain(void)
{
    if (!net_stuck && net_owed_len != 0) {
        net_send_owed();
    }
    for (;;) {
        struct klog_record r;
        uint32_t flags;
        unsigned int n, had;
        int lost, net_lost;
        int i;

        flags = kirq_save();
        if (pending_count() == 0) {
            lost = pending_lost;
            pending_lost = 0;
            net_lost = net_stuck ? 0 : net_owed_dropped;
            net_owed_dropped -= net_lost;
            klog_delivering = 0;
            kirq_restore(flags);
            if (lost > 0) {
                klog(KLOG_WARN, "klog", "dropped: %d record(s) the queue could not hold",
                     (int)lost);
            }
            if (net_lost > 0) {
                klog(KLOG_WARN, "klog",
                     "dropped: %d record(s) for netconsole written while the big lock's "
                     "holder was locked up",
                     net_lost);
            }
            return;
        }
        n = pending_first;
        r = pending[n % KLOG_PENDING_SLOTS];
        pending_writing = 1;
        kirq_restore(flags);

        had = pending_had_by(n);
        for (i = 0; i < KLOG_N_DESTS; i++) {
            if (DESTS[i].write == dest_ring) {
                continue; /* the ring took it where it was raised */
            }
            if ((had & (1u << i)) != 0u) {
                continue; /* it had the record whole before a panic */
            }
            DESTS[i].write(&r);
            had |= 1u << i;
            k_barrier();
            pending_had = (n << 4) | had;
            k_barrier();
        }
        flags = kirq_save();
        pending_first = n + 1u;
        pending_writing = 0;
        kirq_restore(flags);
    }
}

/* ---- the records kept for later ---- */

/*
 * The records made in an NMI or by a processor without the big lock
 * (`smp_serialized`), oldest first. A writer takes the next slot by a
 * compare-and-exchange of `defer_tail`, which an NMI on the same processor
 * may also do, builds the record in it and then marks it whole in
 * `defer_whole`, with its sequence number plus one; the processor that
 * moves the records on takes them from `defer_head` while they are whole
 * and frees each slot by moving `defer_head` past it. A record that finds
 * the queue full is counted and dropped. A hard-lockup report with a
 * backtrace of each of a few other processors is some fifty records.
 */
#define KLOG_DEFER_MAX 128

static struct klog_record deferred[KLOG_DEFER_MAX];
static volatile uint32_t defer_whole[KLOG_DEFER_MAX];
static volatile unsigned int defer_tail;
static volatile unsigned int defer_head;
static volatile unsigned int defer_lost;

/* Set by the panic, which has stopped the other processors: a slot one of
 * them left part built is skipped, since it will never be whole. */
static int defer_skip;

/* The number, plus one, of the processor that writes the kept records out
 * for a holder of the big lock that is locked up (`klog_flush_stuck`), or 0. */
static volatile unsigned int defer_taking;

/* How deep the processor that may write klog's shared state (`klog_enter`)
 * is in doing so, its interrupt handlers' records and console bytes
 * counted: only that one writes it, and it is 0 again before that
 * processor lets the big lock go, since no switch comes while it is not
 * (`sched_preempt`). */
static volatile unsigned int klog_users;

/**
 * Begin writing klog's shared state, the ring, the queue, the kept records'
 * head, the delivery flags and the console's bytes, with interrupts off;
 * `klog_leave` ends it. 1 when this processor may: it may write what the
 * big lock guards (`smp_serialized`) and no other is writing the kept
 * records out for it (`defer_taking`), or it has begun a panic, which
 * stopped the others. 0 otherwise, and the caller keeps its record for
 * later.
 *
 * A holder of the big lock and `klog_flush_stuck` exclude each other as
 * Dekker's algorithm has two processors do: each writes its own word by a
 * locked exchange, a full barrier, and then reads the other's, so at least
 * one of them sees the other and stands back. A holder already in, as an
 * interrupt handler's record finds it, stays in without reading
 * `defer_taking`: the writer reads the count after its own exchange and
 * stands back.
 */
static int klog_enter(void)
{
    unsigned int taking;

    if (!smp_serialized()) {
        return 0;
    }
    if (arch_atomic_xchg(&klog_users, klog_users + 1u) != 0u) {
        return 1;
    }
    taking = defer_taking;
    if (taking != 0u && taking != arch_cpu_id() + 1u && !k_panicking()) {
        klog_users--;
        return 0;
    }
    return 1;
}

/**
 * As `klog_enter`, for the console's bytes, which cannot be kept for later:
 * while another processor writes the kept records out (`klog_flush_stuck`),
 * wait for it to finish, as Linux's printk waits for the console's owner
 * (`console_trylock_spinning`, kernel/printk/printk.c:1956) and its serial
 * console's waits touch the NMI watchdog (drivers/tty/serial/8250/
 * 8250_port.c:2083). The writer's work is bounded: the records kept when it
 * began. Interrupts off, so the wait answers other processors' requests
 * (`smp_service`), since the writer may itself wait for this processor to
 * drop a translation. 1 when in; 0 when this processor may not write
 * klog's state at all, and writes its bytes without it, as before the big
 * lock was taken.
 */
static int klog_enter_wait(void)
{
    while (!klog_enter()) {
        if (!smp_serialized()) {
            return 0;
        }
        while (defer_taking != 0u && !k_panicking()) {
            touch_nmi_watchdog();
            smp_service();
            arch_relax();
        }
    }
    return 1;
}

/** End what a `klog_enter` that returned 1 began. */
static void klog_leave(void)
{
    klog_users--;
}

/**
 * Keep a record made where klog's shared state may not be written, built
 * from `level`, `subsystem`, `format` and `ap` in the next free slot. The
 * caller has interrupts off; an NMI may come in.
 */
static void defer_put(int level, const char *subsystem, const char *format, va_list ap)
{
    unsigned int t, lost;

    do {
        t = defer_tail;
        if (t - defer_head >= KLOG_DEFER_MAX) {
            do {
                lost = defer_lost;
            } while (arch_atomic_cmpxchg(&defer_lost, lost, lost + 1u) != lost);
            return;
        }
    } while (arch_atomic_cmpxchg(&defer_tail, t, t + 1u) != t);
    klog_build(&deferred[t % KLOG_DEFER_MAX], level, subsystem, format, ap);
    k_barrier();
    defer_whole[t % KLOG_DEFER_MAX] = t + 1u;
}

/**
 * Move the kept records before the sequence number `end`, oldest first,
 * into the ring and the queue of records waiting for the other
 * destinations, while that queue has room and the oldest is whole. The
 * caller may write klog's shared state and has interrupts off. The number
 * moved.
 *
 * A record goes to the ring and to the queue each once, though a panic
 * stopped a collector between the two steps, or before `defer_head` moved
 * past it: the ring's state names the last kept record it took
 * (`ring_append`), and the queue's last entry says which kept record it is.
 */
static int defer_collect(unsigned int end)
{
    unsigned int h, slot;
    int moved = 0, queued;

    while ((int)(end - defer_head) > 0) {
        h = defer_head;
        slot = h % KLOG_DEFER_MAX;
        if (defer_whole[slot] == h + 1u) {
            queued = pending_count() > 0 && pending_last()->kept == h + 1u;
            if (!queued && pending_full()) {
                break;
            }
            if (ring_st[ring_cur].kept != h + 1u) {
                ring_append(deferred[slot].text, deferred[slot].len, h + 1u);
                pstore_console_write(deferred[slot].text, deferred[slot].len);
            }
            if (!queued) {
                pending_put(&deferred[slot], h + 1u);
            }
            moved++;
        } else if (!defer_skip) {
            break;
        }
        k_barrier();
        defer_head = h + 1u;
    }
    if (defer_lost != 0) {
        pending_lost += (int)arch_atomic_xchg(&defer_lost, 0u);
    }
    return moved;
}

int klog_kept(void)
{
    return defer_head != defer_tail;
}

int klog_inside(void)
{
    return klog_users != 0u;
}

/**
 * The exchange of `defer_taking` is from 0, so a processor that finds
 * another writing leaves that one's number in place. Netconsole's share is
 * held for the holder (`net_owed`), since the network stack has no guard
 * but the big lock, which this processor does not hold. The records kept when
 * it began are those before the `defer_tail` it reads once it has the
 * kept records to itself: before then the holder, back, may still move
 * `defer_head` past an earlier reading.
 */
void klog_flush_stuck(void)
{
    unsigned int end;
    uint32_t flags;
    int moved;

    if (defer_head == defer_tail ||
        arch_atomic_cmpxchg(&defer_taking, 0u, (unsigned int)arch_cpu_id() + 1u) != 0u) {
        return;
    }
    if (klog_users != 0u) {
        defer_taking = 0; /* the holder is writing klog; it writes them */
        return;
    }
    end = defer_tail;
    net_stuck = 1;
    do {
        flags = kirq_save();
        moved = defer_collect(end);
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
    } while (moved > 0 && (int)(end - defer_head) > 0);
    net_stuck = 0;
    k_barrier();
    defer_taking = 0;
}

/**
 * Nested inside a delivery or a console write, the kept records join the
 * ring and the queue at once, ahead of the record being made, and the
 * delivery or write under way writes them out when it ends; only the drain
 * waits for it. Not nested, records left in the queue, as the panic finds
 * those of a delivery it stopped (`klog_deliver_directly`), go out first,
 * though a full queue took no kept record, and the kept ones after them,
 * so that the panic's own record comes after both, as Linux puts an NMI's
 * record in its ring at once and defers only the console's write of it
 * (`is_printk_legacy_deferred`, kernel/printk/printk_safe.c:41-55), and
 * its panic writes the ring out in order (`console_flush_on_panic`,
 * kernel/printk/printk.c:3449-3482).
 */
void klog_flush(void)
{
    uint32_t flags;
    int nested, moved;

    while (defer_head != defer_tail) {
        flags = kirq_save();
        if (!klog_enter()) {
            kirq_restore(flags);
            return;
        }
        nested = klog_delivering || console_writing;
        moved = defer_collect(defer_tail);
        if (nested || (moved == 0 && pending_count() == 0 && net_owed_len == 0)) {
            klog_leave();
            kirq_restore(flags);
            return;
        }
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
        klog_leave();
    }
    if (net_owed_len != 0) {
        flags = kirq_save();
        if (!klog_enter()) {
            kirq_restore(flags);
            return;
        }
        if (klog_delivering || console_writing || net_owed_len == 0) {
            klog_leave();
            kirq_restore(flags);
            return;
        }
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
        klog_leave();
    }
}

/**
 * The build runs with interrupts off because the formatter writes through
 * sink state that two callers cannot share. The destinations then run with
 * interrupts as this was entered, so a record raised by an interrupt handler
 * can arrive in the middle of one being written out. Such a record goes to
 * the ring where it is raised and waits in the queue for the rest: its bytes
 * inside another record's line, or inside a terminal escape the shell is part
 * way through, would be worse than the wait. The delivery that interrupted it
 * writes it out when its own record is finished.
 *
 * A record made where klog's shared state may not be written is kept for
 * later (`defer_put`); one made where it may comes after those kept before
 * it, which go out first (`klog_flush`), and those kept while it was being
 * written go out after it. Inside a delivery or a console write, a record
 * that finds kept records left over, which the full queue could not take,
 * is kept behind them, so that it does not reach the ring first. A record
 * that finds others queued and no
 * delivery under way, as the panic's first does after it has let the
 * queue go (`klog_deliver_directly`), writes them out first and then goes
 * out itself, since behind a full queue it would be dropped, as Linux's
 * panic writes out every record the consoles have not had
 * (`console_flush_on_panic`, kernel/printk/printk.c:3449-3482).
 */
void klog_v(int level, const char *subsystem, const char *format, va_list ap)
{
    struct klog_record r;
    uint32_t flags;
    int nested, queued, i;

    if (level < KLOG_CRIT) {
        level = KLOG_CRIT;
    }
    if (level > KLOG_DEBUG) {
        level = KLOG_DEBUG;
    }
    klog_flush();
    flags = kirq_save();
    if (!klog_enter()) {
        defer_put(level, subsystem, format, ap);
        kirq_restore(flags);
        return;
    }
    if (!klog_delivering && !console_writing && pending_count() > 0) {
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
        flags = kirq_save();
    }
    nested = klog_delivering || console_writing;
    if (nested && defer_head != defer_tail) {
        (void)defer_collect(defer_tail);
        if (defer_head != defer_tail && pending_full()) {
            klog_leave();
            defer_put(level, subsystem, format, ap);
            kirq_restore(flags);
            return;
        }
    }
    klog_build(&r, level, subsystem, format, ap);
    queued = nested || pending_count() > 0;
    if (queued) {
        pending_put(&r, 0u);
    }
    if (!nested) {
        klog_delivering = 1;
    }
    kirq_restore(flags);

    for (i = 0; i < KLOG_N_DESTS; i++) {
        if (queued && DESTS[i].write != dest_ring) {
            continue;
        }
        DESTS[i].write(&r);
    }
    if (!nested) {
        klog_drain();
        klog_flush();
    }
    klog_leave();
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    klog_v(level, subsystem, format, ap);
    va_end(ap);
}

/* ---- the reports that cannot wait ---- */

int klog_report_begin(void)
{
    return report_get(1);
}

/**
 * The record goes to the ring under the ring's lock, which is held only
 * with interrupts off and for no wait, and then, unless the report is the
 * ring's alone, to the port.
 */
void klog_report(int how, int level, const char *subsystem, const char *format, ...)
{
    struct klog_record r;
    va_list ap;

    va_start(ap, format);
    klog_build(&r, level, subsystem, format, ap);
    va_end(ap);
    ring_append(r.text, r.len, 0u);
    if (how == KLOG_REPORT_RING) {
        return;
    }
    serial_record(&r);
}

void klog_report_end(int how)
{
    report_put(how);
}

/* ---- the console ---- */

/* The console writes of each processor that hold klog's state
 * (`klog_enter_wait`), which `console_write_end` lets go. */
static unsigned int console_held[NR_CPUS];

/**
 * Without the bracket a record raised in the middle of a write ends the open
 * line and begins its own, and a token printed in one write reaches the port
 * as two lines, where a harness looking for the token does not find it. Two
 * writers take the bracket. The tty's write of the console takes it around
 * each chunk of at most 256 bytes that `sys_write` hands it, so a record
 * waits for at most that chunk's bytes, and a token printed within one chunk
 * is one line of the log; a longer write may still have a record between two
 * chunks. The debug shell's `console_puts` and `console_printf` take it
 * around each call, so a line the shell prints in one call is one line of
 * the log.
 *
 * The bracket holds klog's state (`klog_enter_wait`) until it ends, so that
 * a processor writing the kept records out for a locked-up holder
 * (`klog_flush_stuck`) does not begin inside it, and the holder, back,
 * waits for that one to finish before its bytes go out.
 */
void console_write_begin(void)
{
    uint32_t flags;

    klog_flush();
    flags = kirq_save();
    if (klog_enter_wait()) {
        console_held[arch_cpu_id()]++;
    }
    console_writing++;
    textcon_hold();
    kirq_restore(flags);
}

void console_write_end(void)
{
    uint32_t flags = kirq_save();
    unsigned int me = arch_cpu_id();
    int held = console_held[me] > 0u;

    textcon_release();
    if (held) {
        console_held[me]--;
    }
    if (--console_writing > 0 || !held) {
        if (held) {
            klog_leave();
        }
        kirq_restore(flags);
        return;
    }
    if (klog_delivering) {
        klog_leave();
        kirq_restore(flags);
        return;
    }
    if (pending_count() == 0) {
        klog_leave();
        kirq_restore(flags);
        klog_flush();
        return;
    }
    klog_delivering = 1;
    kirq_restore(flags);
    klog_drain();
    klog_leave();
    klog_flush();
}

/**
 * A panic never comes back to drain the queue, so a record of its that
 * joined the queue would reach the ring and nothing else. The panic has
 * stopped the other processors, so a kept record one of them left part
 * built is skipped.
 */
void klog_deliver_directly(void)
{
    uint32_t flags = kirq_save();
    int c;

    klog_delivering = 0;
    console_writing = 0;
    for (c = 0; c < NR_CPUS; c++) {
        console_held[c] = 0;
    }
    klog_users = 0;
    defer_taking = 0;
    net_stuck = 0;
    defer_skip = 1;
    kirq_restore(flags);
}

/** Nothing undoes it, since the panic that stops the screen does not
 * return. */
void klog_screen_stop(void)
{
    screen_stopped = 1;
}

/**
 * `KLOG_CRIT` is the floor so that a critical record reaches the screen
 * whatever a caller asks for: a machine that is about to stop must be able
 * to say so to the person in front of it.
 */
void klog_set_screen_level(int level)
{
    if (level < KLOG_CRIT) {
        level = KLOG_CRIT;
    }
    if (level > KLOG_DEBUG) {
        level = KLOG_DEBUG;
    }
    screen_level = level;
}

void klog_set_ignore_loglevel(int on)
{
    screen_all_levels = on != 0;
}

int klog_ignore_loglevel(void)
{
    return screen_all_levels;
}

void klog_set_time(int on)
{
    show_time = on != 0;
}

int klog_time(void)
{
    return show_time;
}

void klog_set_screen_level_from_cmdline(int level)
{
    klog_set_screen_level(level);
    screen_level_asked_for = 1;
}

int klog_screen_level_was_asked_for(void)
{
    return screen_level_asked_for;
}

int klog_screen_level(void)
{
    return screen_level;
}

/**
 * Hold klog's state for one console byte, so that the byte and the line
 * flags it sets never go out while another processor writes the kept
 * records out; inside a console write, which holds it already, this only
 * nests. 1 when held, for `console_byte_end`.
 */
static int console_byte_begin(void)
{
    uint32_t flags = kirq_save();
    int took = klog_enter_wait();

    kirq_restore(flags);
    return took;
}

/** Let go what `console_byte_begin` took when it returned 1 in `took`. */
static void console_byte_end(int took)
{
    if (took) {
        klog_leave();
    }
}

/**
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
    int took = console_byte_begin();

    screen_line_open = c != '\n';
    serial_line_open = c != '\n';
    k_putc(c);
    if (c == '\n') {
        serial_out('\r');
    }
    serial_out(c);
    console_byte_end(took);
}

/**
 * The line discipline expands a newline to carriage return and newline before
 * it reaches here, so this must pass every byte through unchanged; expanding
 * again would put two carriage returns on the wire for every line.
 */
void console_putc_raw(char c)
{
    int took = console_byte_begin();

    screen_line_open = c != '\n';
    serial_line_open = c != '\n';
    k_putc(c);
    serial_out(c);
    console_byte_end(took);
}

/**
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
    console_write_begin();
    while (*str != '\0') {
        console_putc(*str++);
    }
    console_putc('\n');
    console_write_end();
}

void console_printf(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    console_write_begin();
    k_vprintf(console_putc, format, ap);
    console_write_end();
    va_end(ap);
}
