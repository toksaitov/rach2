/*
 * panic_hosttest.c: the panic's picture and short report on the screen
 * (`panic.c`), on the text console and on framebuffers of three sizes.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c rach2/sys/wcwidth.c
 * host-deps: rach2/sys/tick.c
 *
 * The console, the log and the panic are included whole, and `k_stop` jumps
 * back to the row, so each row can read the cells the panic left. A row raises
 * the kernel exception's order, the record, the note and then `k_panic`, or
 * the hardware fault's, `k_panic_begin` and then the record. The screens are
 * the VGA's 80 by 25 and 80 by 24, the smallest the console takes, the
 * Lenovo's 85 by 24, where the report goes over the picture's bottom-left
 * corner, and 128 by 48, where it goes below the picture. `k_udelay` counts
 * what the panic waits and `power_emergency_restart` jumps back too, so that
 * a row of `panic_timeout` reads the wait, the countdown and the reset.
 */
#include <setjmp.h>

#include "lib/kint.h"

#define VGA_TEXT_HOSTTEST

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram

extern int printf(const char *format, ...);
extern void *memset(void *s, int c, unsigned long n);
extern char *strstr(const char *haystack, const char *needle);
extern char *strchr(const char *s, int c);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern unsigned long strlen(const char *s);

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

static uint8_t big[1366 * 4 * 768]; /* the framebuffer, at most the Lenovo's size */

/** The framebuffer's mapping. */
volatile uint8_t *fbcon_window(void)
{
    return big;
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

#define CHECK(condition, detail)                                 \
    do {                                                         \
        if (!(condition)) {                                      \
            printf("panic-test: fail: %s: %s\n", where, detail); \
            failures++;                                          \
        }                                                        \
    } while (0)

static char shown[TEXTCON_COLS_MAX * 64 + 1]; /* the screen, its rows run together */
static process_t running;
static const char *where = "80 by 25"; /* the screen the rows run on */
static int dumps;                      /* what `dump` returns */

/** The cell at (`col`, `row`) of the live screen. */
static uint16_t cell_at(unsigned int col, unsigned int row)
{
    return vga_text_line_ptr(_live_top_line + row)[col];
}

/** Copy the screen's characters into `shown`, a row after another. */
static void read_screen(void)
{
    unsigned int col, row, i = 0;

    for (row = 0; row < textcon_rows; row++) {
        for (col = 0; col < textcon_cols; col++) {
            char c = (char)(cell_at(col, row) & 0xFF);

            shown[i++] = c == '\0' ? ' ' : c;
        }
    }
    shown[i] = '\0';
}

/** Whether `text` begins a row of the screen at column `col`; the row, or
 * -1. */
static int row_of(const char *text, unsigned int col)
{
    unsigned int row, n = (unsigned int)strlen(text);

    for (row = 0; row < textcon_rows; row++) {
        if (col + n <= textcon_cols && memcmp(shown + row * textcon_cols + col, text, n) == 0) {
            return (int)row;
        }
    }
    return -1;
}

/** A crash record that is saved, or not, as `dumps` says. */
static int dump(void)
{
    return dumps;
}

/** A log and a panic not yet raised, with the screen clear. */
static void restart(void)
{
    panic_owner = 0;
    others_stopped = 0;
    screen_stopped = 0;
    fault_name = 0;
    fault_has_address = 0;
    reading_process = 0;
    process_unsafe = 0;
    wire_len = 0;
    wire[0] = '\0';
    screen_line_open = 0;
    serial_line_open = 0;
    klog_delivering = 0;
    console_writing = 0;
    waited_us = 0;
    restarted = 0;
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

/**
 * Whether the picture is whole and no cell of the character is covered:
 * every row from her first is on the screen, every cell of hers shows her
 * color with nothing written in it, and nothing scrolled.
 */
static int picture_whole(void)
{
    unsigned int col, row;
    int r;

    if (_live_top_line != 0 || report.picture_top + FAIL_FIRST_ROW < 0 ||
        report.picture_top + (int)FAIL_ROWS > (int)textcon_rows) {
        return 0;
    }
    for (row = 0; row < FAIL_ROWS; row++) {
        r = report.picture_top + (int)row;
        for (col = 0; col < FAIL_COLS && r >= 0; col++) {
            uint16_t c = cell_at(report.picture_left + col, (unsigned int)r);

            if (_fail_image[row * FAIL_COLS + col] != blue &&
                ((c >> 12) != _fail_image[row * FAIL_COLS + col] || (c & 0xFF) != ' ')) {
                return 0;
            }
        }
    }
    return 1;
}

/** Whether the lines `lines`, NULL-ended, are rows of the report in that
 * order, one under another, from its left column, and the last is the
 * report's last. */
static int report_reads(const char *const *lines)
{
    int row = -1, r;

    for (; *lines != 0; lines++) {
        r = row_of(*lines, report.left);
        if (r < 0 || (row >= 0 && r != row + 1)) {
            return 0;
        }
        row = r;
    }
    return row == (int)(report.top + report.count - 1);
}

/** A fault in the kernel, its record and its note, and the panic. */
static void exception(void)
{
    restart();
    klog(KLOG_CRIT, "isr", "kernel exception: page fault (vector 14, err 0x2, rip 0x106000)");
    k_panic_note_fault("page fault", 0x106000, 2);
    k_panic_note_address(0xdead0);
    stop("Unrecoverable error: page fault while in kernel mode.");
}

/** The address `v` as the report writes it. */
static const char *address(const char *what, uintptr_t v)
{
    static char line[2][64];
    static int which;
    char *s = line[which++ & 1];

    s[0] = '\0';
    put_str(s, what);
    put_address(s, v);
    return s;
}

/** Check the report of a page fault and a hardware fault on the screen as it
 * is set. */
static void routes(const char *what)
{
    const char *fault[] = {"rebooting in 0 s",
                           "build " RACH2_HEAD,
                           "cpu 0  pid 42",
                           "cmd ?[2J?[H",
                           0,
                           0,
                           "page fault, error 0x2",
                           "panic: Unrecoverable error: page fault while in kernel mode.",
                           0};
    const char *nmi[] = {"halted",
                         "build " RACH2_HEAD,
                         "cpu 0  pid 42",
                         "cmd ?[2J?[H",
                         0,
                         "non-maskable interrupt",
                         "panic: the machine reported a fault",
                         0};

    where = what;
    panic_timeout = PANIC_TIMEOUT_DEFAULT;
    exception();
    fault[4] = address("ip ", 0x106000);
    fault[5] = address("addr ", 0xdead0);
    CHECK(picture_whole(), "the picture is whole, with no cell of hers covered");
    CHECK(report_reads(fault), "the report reads its lines in order, the reason last");
    CHECK(waited_us == 30000000ul && restarted, "the default waits 30 seconds and resets");
    CHECK(strstr(wire, "panic: stopped: Unrecoverable error") != 0 &&
              strstr(wire, "panic: report: cpu 0  pid 42") != 0 &&
              strstr(wire, "panic: rebooting in 30 seconds..") != 0,
          "the port has the record, the report's lines and the reset's line");
    CHECK(strchr(wire, '\033') == 0, "and no escape from the process's name");
    CHECK(strstr(shown, "isr:") == 0 && strstr(shown, "stopped") == 0,
          "the screen has no record of the log's");

    restart();
    panic_timeout = 0;
    k_panic_begin();
    klog_deliver_directly();
    klog(KLOG_CRIT, "isr", "hardware fault: non-maskable interrupt (vector 2, pc 0x%lx)",
         (unsigned long)0x106000);
    k_panic_note_fault("non-maskable interrupt", 0x106000, PANIC_NO_ERROR);
    stop("the machine reported a fault");
    nmi[4] = address("ip ", 0x106000);
    CHECK(picture_whole(), "the hardware fault's picture is whole");
    CHECK(report_reads(nmi), "and its report has no error code or address, and says halted");
    CHECK(!restarted && waited_us == 0, "panic=0 halts");
    panic_timeout = PANIC_TIMEOUT_DEFAULT;
}

/** The countdown, the other timeouts, the saved record, a long reason, a
 * fault while the process is read, and a panic with no process. */
static void edges(void)
{
    static char lengthy[1024];
    const char *saved[] = {"rebooting in 0 s", "crash record saved", "build " RACH2_HEAD, 0};
    const char *bare[] = {"rebooting", "build " RACH2_HEAD, "cpu 0", "panic: no process", 0};

    restart();
    panic_timeout = 3;
    stop("timed");
    CHECK(waited_us == 3000000ul && restarted &&
              row_of("rebooting in 0 s", report.left) == (int)report.top &&
              row_of("rebooting in 0 s ", report.left) >= 0,
          "panic=3 counts down to 0 and resets, and a shorter count leaves no digit");
    CHECK(strstr(wire, "panic: rebooting in 3 seconds..") != 0, "and says so on the port");

    restart();
    dumps = 1;
    k_panic_dump = dump;
    stop("saved");
    CHECK(row_of(saved[0], report.left) == (int)report.top &&
              row_of(saved[1], report.left) == (int)report.top + 1,
          "a saved crash record is said under the countdown");
    k_panic_dump = 0;

    k_memset(lengthy, 'r', sizeof(lengthy) - 1);
    restart();
    stop(lengthy);
    CHECK(picture_whole(), "a long reason covers none of her");
    CHECK(row_of("panic: rrrr", report.left) == (int)(report.top + report.count - 1),
          "and is cut on its row");

    restart();
    panic_timeout = -1;
    reading_process = 1;
    stop("no process");
    CHECK(restarted && waited_us == 0 && strstr(wire, "rebooting in") == 0,
          "panic=-1 resets at once, with no countdown");
    CHECK(report_reads(bare) && strstr(shown, "pid") == 0,
          "a panic while the process was read leaves it out");
    panic_timeout = PANIC_TIMEOUT_DEFAULT;
}

/** Take a framebuffer of `w` by `h` pixels at `big`. */
static int framebuffer(unsigned int w, unsigned int h)
{
    static struct boot_framebuffer fb;

    fb.type = BOOT_FB_RGB;
    fb.addr = 0x80000000u;
    fb.width = w;
    fb.height = h;
    fb.bpp = 32;
    fb.pitch = w * 4;
    fb.red_shift = 16;
    fb.red_bits = 8;
    fb.green_shift = 8;
    fb.green_bits = 8;
    fb.blue_shift = 0;
    fb.blue_bits = 8;
    return fbcon_init(&fb);
}

int main(void)
{
    routes("the text console at 80 by 25");
    edges();
    textcon_rows = 24;
    routes("the text console at 80 by 24");

    CHECK(framebuffer(1366, 768) == 0 && textcon_cols == 85 && textcon_rows == 24,
          "the Lenovo's framebuffer is 85 by 24");
    routes("the framebuffer at 85 by 24");
    CHECK(report.top + report.count <= FAIL_ROWS + (unsigned int)report.picture_top,
          "where the report goes over the picture");

    CHECK(framebuffer(1024, 768) == 0 && textcon_cols == 128 && textcon_rows == 48,
          "a framebuffer of 128 by 48");
    routes("the framebuffer at 128 by 48");
    CHECK((int)report.top == report.picture_top + (int)FAIL_ROWS + 1 &&
              report.left == report.picture_left,
          "where the report goes below the picture, under its left edge");

    if (failures != 0) {
        printf("panic-test: result: fail: %d\n", failures);
        return 1;
    }
    printf("panic_hosttest: PASS\n");
    return 0;
}
