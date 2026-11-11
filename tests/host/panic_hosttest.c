/*
 * panic_hosttest.c: the panic's report on the screen (`panic.c`), at 80 by 24
 * on the text console and on a framebuffer.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c rach2/sys/wcwidth.c
 * host-deps: rach2/sys/tick.c
 *
 * The console, the log and the panic are included whole, and `k_stop` jumps
 * back to the row, so each row can read the cells the panic left. A row raises
 * the kernel exception's order, the record and then `k_panic`, or the hardware
 * fault's, `k_panic_begin` and then the record. The rows at 80 by 24 are the
 * smallest screen the console takes, where the report covers the picture; the
 * QEMU rows of machine.smp force the two faults on 80 by 25. `k_udelay`
 * counts what the panic waits and `power_emergency_restart` jumps back too,
 * so that a row of `panic_timeout` reads the wait and the reset.
 */
#include <setjmp.h>

#include "lib/kint.h"

#define VGA_TEXT_HOSTTEST

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram

extern int printf(const char *format, ...);
extern void *memset(void *s, int c, unsigned long n);
extern char *strstr(const char *haystack, const char *needle);

/** The adapter's store hook of vga_text.c, which these rows do not count. */
void vga_text_hosttest_stored(void)
{}

/** No interrupts here: nothing to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

#include "display/vga_gfx.h"
bool vga_graphics_mode = false;

#include "display/vga_text.c"
#include "display/fbcon.c"
#include "display/vga_font.c"
#include "kio.c"
#include "klog.c"
#include "panic.c"
#include "proc/process.h"

struct percpu cpu_table[NR_CPUS];
volatile uint64_t kernel_ticks;

static char wire[8192]; /* what reached the serial port and netconsole */
static unsigned int wire_len;

/** Record a byte of the serial port. */
void serial_out(char c)
{
    if (wire_len + 1 < sizeof(wire)) {
        wire[wire_len++] = c;
        wire[wire_len] = '\0';
    }
}

/** Drop a byte of netconsole: `dest_net` takes what `dest_serial` takes. */
void netconsole_putc(char c)
{
    (void)c;
}

/** Nothing here is off yet. */
void arch_irq_disable(void)
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

/** No other processor here. */
void smp_stop_others(void)
{}

/** The console here never blanks. */
void console_panic_unblank(void)
{}

/** The adapter's font load, which a framebuffer console does not make. */
int vga_font_load(void)
{
    return 0;
}

/** A write to the adapter's ports, which reach no device here. */
void outb(uint16_t port, uint8_t val)
{
    (void)port;
    (void)val;
}

/** A read of the adapter's ports, which reach no device here. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

/** The blank time the escape sets, which no row reads. */
void console_set_blank_time(unsigned int seconds)
{
    (void)seconds;
}

static uint8_t pixels[(640 * 4 + 64) * 384]; /* the framebuffer */

/** The framebuffer's mapping. */
volatile uint8_t *fbcon_window(void)
{
    return pixels;
}

static jmp_buf stop_point; /* where `k_stop` goes back to */

/** The panic's end: back to the row. */
void k_stop(void)
{
    longjmp(stop_point, 1);
}

static unsigned long waited_us; /* what the panic waited, by `k_udelay` */
static int restarted;           /* whether the panic reset the machine */

/** Add `usec` to the time waited, without waiting. */
void k_udelay(uint32_t usec)
{
    waited_us += usec;
}

/** The panic's reset: counted, and back to the row. */
void power_emergency_restart(void)
{
    restarted = 1;
    longjmp(stop_point, 2);
}

/** No boot line here: no option is given. */
int kcmdline_count(const char *key)
{
    (void)key;
    return 0;
}

/** No flag is given. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** No option has a value. */
int kcmdline_value_nth(const char *key, int nth, char *out, unsigned int outlen)
{
    (void)key;
    (void)nth;
    if (outlen > 0) {
        out[0] = '\0';
    }
    return 0;
}

static int failures;

#define CHECK(condition, detail)                      \
    do {                                              \
        if (!(condition)) {                           \
            printf("panic-test: fail: %s\n", detail); \
            failures++;                               \
        }                                             \
    } while (0)

static char shown[TEXTCON_COLS_MAX * 64]; /* the screen, its rows run together */
static process_t running;

/** Copy the screen's cells into `shown`. */
static void read_screen(void)
{
    unsigned int i, cells = textcon_cols * textcon_rows;

    for (i = 0; i < cells; i++) {
        char c = (char)(vga_text_memory()[i] & 0xFF);

        shown[i] = c == '\0' ? ' ' : c;
    }
    shown[cells] = '\0';
}

/** A log and a panic not yet raised, with the screen clear. */
static void restart(void)
{
    panic_owner = 0;
    others_stopped = 0;
    last_crit.len = 0;
    wire_len = 0;
    wire[0] = '\0';
    screen_line_open = 0;
    serial_line_open = 0;
    klog_delivering = 0;
    console_writing = 0;
    set_back_color(black);
    k_cls();
    cpu_table[0].current = &running;
    running.pid = 42;
    k_memcpy(running.comm, "\033[2J\033[H", 8);
}

/** Stop with `reason`, and read the screen. */
static void stop(const char *reason)
{
    if (setjmp(stop_point) == 0) {
        k_panic(reason);
    }
    read_screen();
}

/** Say whether nothing scrolled: the picture's top and the last row are as
 * the panic drew them, and the report left the cursor on the screen. */
static int stayed(void)
{
    unsigned int last = (textcon_rows - 1) * textcon_cols;
    unsigned int col;

    for (col = 0; col < textcon_cols; col++) {
        if (shown[last + col] != ' ') {
            return 0;
        }
    }
    return _live_top_line == 0 && y == textcon_rows - 1;
}

/** Check the report of the two routes on the screen as it is set. */
static void both_routes(const char *what)
{
    const char *cause, *reason;

    restart();
    klog(KLOG_CRIT, "isr",
         "kernel exception: invalid opcode (vector 6, err 0x0, rip 0xffffffff80106000)");
    stop("unrecoverable CPU exception in kernel mode");
    cause = strstr(shown, "isr: kernel exception: invalid opcode (vector 6,");
    CHECK(cause != 0, what);
    reason = cause != 0 ? strstr(cause, "panic: stopped: unrecoverable CPU") : 0;
    CHECK(reason != 0, what);
    CHECK(stayed(), what);
    CHECK(strstr(shown, "pid 42") == 0 && strstr(wire, "pid 42") == 0,
          "the report reads no process, whose name clears the screen");
    CHECK(strstr(wire, "kernel exception") == strstr(wire, "isr: kernel") + 5,
          "the port has the record once");
    CHECK(strstr(strstr(wire, "kernel exception") + 1, "kernel exception") == 0, "and only once");

    restart();
    k_panic_begin();
    klog_deliver_directly();
    klog(KLOG_CRIT, "isr", "hardware fault: non-maskable interrupt (vector 2, pc 0x%lx)",
         (unsigned long)0x106000);
    stop("the machine reported a fault");
    cause = strstr(shown, "isr: hardware fault: non-maskable interrupt (vector 2, pc 0x106000)");
    CHECK(cause != 0, what);
    reason = cause != 0 ? strstr(cause, "panic: stopped: the machine reported") : 0;
    CHECK(reason != 0, what);
    CHECK(stayed(), what);
}

/**
 * What `panic_timeout` does once the record is out: 3 says when the machine
 * resets, on the screen and the port, waits 3 seconds and resets, and the
 * longest record and reason still fit with that line; -1 resets with no wait
 * and no line; 0 halts with neither.
 */
static void timeouts(void)
{
    static char lengthy[1024];

    restart();
    panic_timeout = 3;
    waited_us = 0;
    restarted = 0;
    klog(KLOG_CRIT, "isr", "kernel exception: before a reset");
    stop("timed");
    CHECK(strstr(wire, "panic: rebooting in 3 seconds..") != 0 &&
              strstr(shown, "panic: rebooting in 3 seconds..") != 0,
          "panic=3 says when the machine resets, on the screen and the port");
    CHECK(waited_us == 3000000ul && restarted, "and resets it after waiting 3 seconds");
    CHECK(stayed(), "and its line scrolls nothing");

    k_memset(lengthy, 'r', sizeof(lengthy) - 1);
    restart();
    klog(KLOG_CRIT, "isr", "%s", lengthy);
    stop(lengthy);
    CHECK(stayed() && strstr(shown, "panic: rebooting in 3 seconds..") != 0,
          "the longest record and reason fit with the reset's line");

    restart();
    panic_timeout = -1;
    waited_us = 0;
    restarted = 0;
    stop("at once");
    CHECK(restarted && waited_us == 0 && strstr(wire, "rebooting in") == 0,
          "panic=-1 resets the machine at once, with no line");

    restart();
    panic_timeout = 0;
    restarted = 0;
    stop("halted");
    CHECK(!restarted && waited_us == 0 && strstr(wire, "rebooting in") == 0,
          "panic=0 halts, with no line and no reset");
}

/** The longest record and reason, a panic with no record, one in the middle
 * of a line, and one inside an escape string. */
static void edges(void)
{
    static char lengthy[1024];
    const char *escape = "\033]0;ab";

    k_memset(lengthy, 'r', sizeof(lengthy) - 1);
    restart();
    klog(KLOG_CRIT, "isr", "%s", lengthy);
    stop(lengthy);
    CHECK(stayed(), "the longest record and reason fit without a scroll");
    CHECK(strstr(shown, "...") != 0, "and the cut record shows its mark");
    CHECK(strstr(shown, "panic: stopped: rrr") != 0, "and the reason is there");

    restart();
    stop("no record before");
    CHECK(strstr(shown, "panic: stopped: no record before") != 0, "a panic with no record");
    CHECK(strstr(shown, "isr:") == 0 && stayed(), "shows its reason alone");

    restart();
    console_putc('>');
    klog(KLOG_CRIT, "isr", "kernel exception: after a prompt");
    stop("open line");
    CHECK(stayed(), "a line left open adds no row");
    CHECK(strstr(shown, "isr: kernel exception: after a prompt") != 0, "and the record is shown");

    restart();
    while (*escape != '\0') {
        k_putc(*escape++);
    }
    klog(KLOG_CRIT, "isr", "kernel exception: inside an escape string");
    stop("inside an escape string");
    CHECK(strstr(shown, "isr: kernel exception: inside an escape string") != 0,
          "a half-read escape string takes none of the report");
    CHECK(strstr(shown, "panic: stopped: inside an escape string") != 0, "or of the reason");
}

int main(void)
{
    static struct boot_framebuffer fb;

    textcon_rows = 24;
    both_routes("the text console at 80 by 24 shows the vector, address and reason");
    edges();
    timeouts();

    fb.type = BOOT_FB_RGB;
    fb.addr = 0x80000000u;
    fb.width = 640;
    fb.height = 384;
    fb.bpp = 32;
    fb.pitch = 640 * 4 + 64;
    fb.red_shift = 16;
    fb.red_bits = 8;
    fb.green_shift = 8;
    fb.green_bits = 8;
    fb.blue_shift = 0;
    fb.blue_bits = 8;
    CHECK(fbcon_init(&fb) == 0, "the framebuffer is taken");
    CHECK(textcon_cols == 80 && textcon_rows == 24, "and is 80 by 24");
    both_routes("the framebuffer at 80 by 24 shows the vector, address and reason");

    if (failures != 0) {
        printf("panic-test: result: fail: %d\n", failures);
        return 1;
    }
    printf("panic_hosttest: PASS\n");
    return 0;
}
