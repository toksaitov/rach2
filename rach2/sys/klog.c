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
 * Show the record from its body on, without the priority and the timestamp.
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

    if (r->level > screen_level || screen_stopped) {
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
 * buffer once and drops the rest.
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

static const struct klog_dest DESTS[] = {{KLOG_DEBUG, dest_ring},
                                         {KLOG_INFO, dest_screen},
                                         {KLOG_DEBUG, dest_serial},
                                         {KLOG_DEBUG, dest_net}};

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
 * Sixty-four records hold four for each. One more is counted and dropped,
 * and the count is reported once the queue is empty, because a log that
 * quietly loses records is worse than one that says how many.
 */
#define KLOG_PENDING_MAX 64

static struct klog_record pending[KLOG_PENDING_MAX];
static int pending_head;
static int pending_count;
static int pending_lost;

/**
 * Put a record at the end of the queue, or count it as lost; `kept` is the
 * sequence number, plus one, of the kept record it is, or 0.
 *
 * The caller has interrupts off: this runs inside the same region that
 * built the record and read `klog_delivering`. The count moves after the
 * copy, so a panic that stops it finds the record queued whole or not.
 */
static void pending_put(const struct klog_record *r, uint32_t kept)
{
    struct klog_record *q;

    if (pending_count >= KLOG_PENDING_MAX) {
        pending_lost++;
        return;
    }
    q = &pending[(pending_head + pending_count) % KLOG_PENDING_MAX];
    *q = *r;
    q->kept = kept;
    k_barrier();
    pending_count++;
}

/**
 * Write the queued records to the destinations past the ring, in the order
 * they were raised, and then let the next record be delivered directly.
 *
 * `klog_delivering` stays set for the whole drain, so a record raised by an
 * interrupt during it joins the queue instead of the line being written. The
 * flag is cleared with the queue empty and interrupts off, which leaves no
 * moment where a record is waiting and nobody is coming for it.
 */
static void klog_drain(void)
{
    for (;;) {
        struct klog_record r;
        uint32_t flags;
        int lost;
        int i;

        flags = kirq_save();
        if (pending_count == 0) {
            lost = pending_lost;
            pending_lost = 0;
            klog_delivering = 0;
            kirq_restore(flags);
            if (lost > 0) {
                klog(KLOG_WARN, "klog", "dropped: %d record(s) the queue could not hold",
                     (int)lost);
            }
            return;
        }
        r = pending[pending_head];
        pending_head = (pending_head + 1) % KLOG_PENDING_MAX;
        pending_count--;
        kirq_restore(flags);

        for (i = 0; i < KLOG_N_DESTS; i++) {
            if (DESTS[i].write == dest_ring) {
                continue; /* the ring took it where it was raised */
            }
            if (r.level <= DESTS[i].takes_down_to) {
                DESTS[i].write(&r);
            }
        }
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
 * began. Interrupts off. 1 when in; 0 when this processor may not write
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

    while (defer_head != end) {
        h = defer_head;
        slot = h % KLOG_DEFER_MAX;
        if (defer_whole[slot] == h + 1u) {
            queued = pending_count > 0 &&
                     pending[(pending_head + pending_count - 1) % KLOG_PENDING_MAX].kept == h + 1u;
            if (!queued && pending_count >= KLOG_PENDING_MAX) {
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
 * another writing leaves that one's number in place.
 */
void klog_flush_stuck(void)
{
    unsigned int end = defer_tail;
    uint32_t flags;
    int moved;

    if (defer_head == end ||
        arch_atomic_cmpxchg(&defer_taking, 0u, (unsigned int)arch_cpu_id() + 1u) != 0u) {
        return;
    }
    if (klog_users != 0u) {
        defer_taking = 0; /* the holder is writing klog; it writes them */
        return;
    }
    do {
        flags = kirq_save();
        moved = defer_collect(end);
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
    } while (moved > 0 && (int)(end - defer_head) > 0);
    k_barrier();
    defer_taking = 0;
}

/**
 * Nested inside a delivery or a console write, the kept records join the
 * ring and the queue at once, ahead of the record being made, and the
 * delivery or write under way writes them out when it ends; only the drain
 * waits for it.
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
        if (nested || moved == 0) {
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
    if (!klog_delivering && !console_writing && pending_count > 0) {
        klog_delivering = 1;
        kirq_restore(flags);
        klog_drain();
        flags = kirq_save();
    }
    nested = klog_delivering || console_writing;
    if (nested && defer_head != defer_tail) {
        (void)defer_collect(defer_tail);
        if (defer_head != defer_tail && pending_count >= KLOG_PENDING_MAX) {
            klog_leave();
            defer_put(level, subsystem, format, ap);
            kirq_restore(flags);
            return;
        }
    }
    klog_build(&r, level, subsystem, format, ap);
    queued = nested || pending_count > 0;
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
        if (level <= DESTS[i].takes_down_to) {
            DESTS[i].write(&r);
        }
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
    if (pending_count == 0) {
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
