/*
 * panic.c: the kernel's stop on an error it cannot recover from, with the
 * picture it draws and the record that says why.
 */
#include "panic.h"

#include "arch/arch.h"
#include "cmdline.h"
#include "display/blank.h"
#include "klog.h"
#include "lib/kstd.h"
#include "lib/kstring.h"
#include "power.h"
#include "proc/smp.h"
#include "udelay.h"

/* The panic picture, 80 columns by 25 rows, a background color for each
 * cell. */
#define FAIL_COLS 80u
#define FAIL_ROWS 25u
static const display_color_t _fail_image[FAIL_COLS * FAIL_ROWS] = {
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  0,  15, 12, 0,  1,  1,  0,  0,  1,  1,  0,  0,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 12, 12, 12, 0,  0,  12, 12, 0,
    0,  12, 12, 0,  1,  1,  0,  0,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  12,
    12, 12, 0,  12, 12, 15, 15, 12, 12, 12, 12, 12, 12, 0,  15, 12, 0,  1,  1,  1,  0,  1,  1,  1,
    1,  1,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  0,  15, 12, 15, 12, 15, 15, 15, 15, 12, 12, 15, 12, 0,  15, 12, 12,
    12, 0,  1,  0,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  12, 12, 15, 15, 15, 15, 4,  4,  15,
    15, 15, 15, 15, 0,  12, 12, 12, 12, 0,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  12,
    12, 12, 15, 12, 15, 4,  4,  15, 12, 12, 15, 12, 12, 0,  12, 12, 0,  1,  1,  1,  1,  0,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  0,  15, 15, 12, 0,  12, 12, 12, 0,  12, 12, 0,  12, 0,  12, 12, 0,  12, 12, 12, 0,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 12, 12, 12, 0,  12, 0,  12, 0,  12, 0,  15, 0,  15,
    0,  12, 0,  12, 0,  12, 12, 12, 0,  0,  0,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 15, 15, 12, 0,  1,  0,
    15, 0,  15, 0,  15, 4,  15, 4,  15, 0,  15, 0,  15, 15, 0,  12, 12, 12, 12, 0,  0,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,
    15, 12, 12, 12, 0,  1,  1,  0,  15, 15, 15, 15, 15, 15, 4,  15, 15, 15, 15, 15, 0,  15, 0,  0,
    15, 12, 12, 12, 12, 0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  0,  15, 12, 12, 0,  0,  1,  1,  1,  1,  0,  15, 15, 0,  0,  15, 15, 15,
    0,  0,  15, 15, 15, 15, 0,  1,  0,  15, 15, 12, 12, 12, 12, 0,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 12, 0,  1,  1,  1,  1,  1,  1,
    0,  15, 0,  15, 15, 0,  15, 0,  15, 15, 0,  15, 15, 0,  1,  1,  1,  0,  0,  15, 15, 12, 12, 12,
    0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 12,
    0,  1,  1,  1,  1,  1,  1,  1,  0,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 0,  1,  1,
    1,  1,  1,  0,  0,  15, 12, 12, 0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  0,  15, 12, 0,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 0,  1,  1,  1,  1,  1,  1,  1,  0,  0,  15, 12, 0,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 0,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  0,  0,  15, 15, 4,  4,  15, 15, 15, 0,  1,  1,  1,  15, 1,  1,  1,  1,  1,  0,  15, 0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  4,  4,  0,  0,  0,  1,  1,  1,  0,  15,
    1,  1,  15, 1,  1,  0,  15, 0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  0,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  4,  4,
    1,  1,  1,  1,  1,  1,  0,  15, 1,  15, 0,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  15, 15, 1,  1,  1,  1,  1,  1,  0,  15, 15, 0,  1,  1,  1,  1,  0,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,
    1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  15, 0,
    15, 15, 0,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  0,  15, 15, 0,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1};

/* Halt this processor for good, with interrupts off (loader.s). */
extern void k_stop(void) K_NORETURN;

/* The step of a panic's wait before its reset, in milliseconds, as Linux's
 * PANIC_TIMER_STEP (kernel/panic.c). */
#define PANIC_TIMER_STEP 100

int panic_timeout;
int panic_on_oops;

/**
 * Linux reads `panic=` by `param_set_int`, which takes what `kstrtoint` in
 * base 0 takes and refuses the rest with the old value kept
 * (kernel/params.c); `k_strtoll` reads the same forms, and the range check
 * makes it an int's. `oops=` reads only `panic` (kernel/panic.c,
 * `oops_setup`), and another value changes nothing, so one `oops=panic`
 * anywhere on the line is the whole of it.
 */
void k_panic_setup(void)
{
    char value[24];
    long long v;
    int nth;

    for (nth = 0; nth < kcmdline_count("panic"); nth++) {
        if (!kcmdline_value_nth("panic", nth, value, (unsigned int)sizeof(value)) ||
            k_strtoll(value, k_strlen(value), &v) != 0 || v < -0x7FFFFFFFL - 1 || v > 0x7FFFFFFFL) {
            klog(KLOG_WARN, "panic", "panic=: refused: not an integer");
            continue;
        }
        panic_timeout = (int)v;
    }
    if (kcmdline_has("oops=panic")) {
        panic_on_oops = 1;
    }
    if (panic_timeout != 0) {
        klog(KLOG_INFO, "panic", "timeout: %d seconds", panic_timeout);
    }
}

/* The first processor to panic, as its number plus one, 0 before any has;
 * and whether it has stopped the others. */
static volatile unsigned int panic_owner;
static volatile int others_stopped;

void (*k_panic_hook)(void);

/**
 * Only the first processor to panic reports. A second, which an NMI sent to
 * every processor at once can make, or a panic the stop request did not
 * reach in time, stops without a word, as Linux's does: two records would
 * be printed over each other, and neither holds a lock. The one that
 * reports writes the console without its lock (`console_lock`), which the
 * code it interrupted may hold, or be between taking and marking as its
 * own, and unblanks it first (`console_panic_unblank`), since a blank
 * console draws nothing.
 *
 * The owner is claimed with its number in one compare-and-exchange, since an
 * NMI that no masking keeps out can panic the owner again between any two of
 * its steps: the nested panic finds itself the owner, and stops the others
 * if the first has not yet.
 */
void k_panic_begin(void)
{
    unsigned int me = arch_cpu_id() + 1u;
    unsigned int owner;

    arch_irq_disable();
    if (k_panic_hook) {
        k_panic_hook();
    }
    owner = arch_atomic_cmpxchg(&panic_owner, 0u, me);
    if (owner != 0u && owner != me) {
        k_stop();
    }
    if (!others_stopped) {
        smp_stop_others();
        others_stopped = 1;
    }
    console_panic_unblank();
}

int k_panicking(void)
{
    return panic_owner == arch_cpu_id() + 1u;
}

/**
 * Clear the screen to the picture's background, draw the picture at the
 * screen's center, and return the row that shows the picture's last row.
 *
 * The console is at least 80 by 24 (fbcon.c); on 24 rows the picture loses
 * its first row, which is blank.
 */
static unsigned int draw_fail_image(void)
{
    unsigned int left = 0, top = 0, skip = 0;
    unsigned int col, row;

    if (textcon_cols > FAIL_COLS) {
        left = (textcon_cols - FAIL_COLS) / 2;
    }
    if (textcon_rows >= FAIL_ROWS) {
        top = (textcon_rows - FAIL_ROWS) / 2;
    } else {
        skip = FAIL_ROWS - textcon_rows;
    }

    set_back_color(blue);
    k_cls();
    for (row = skip; row < FAIL_ROWS; ++row) {
        for (col = 0; col < FAIL_COLS; ++col) {
            textcon_write_cell(left + col, top + row - skip, ' ',
                               (uint8_t)(_fail_image[row * FAIL_COLS + col] << 4));
        }
    }
    return top + FAIL_ROWS - 1 - skip;
}

/** The rows that `chars` characters take on the screen, at least one. */
static unsigned int text_rows(size_t chars)
{
    return chars == 0 ? 1 : (unsigned int)((chars + textcon_cols - 1) / textcon_cols);
}

/**
 * Place the cursor for the report of `str`, which is `klog_show_crit`'s
 * record, the line `k_panic` writes and, with `panic_timeout` above 0, the
 * line that says when the machine resets, on the picture's last rows or as
 * far above them as the report needs. Its last line ends with a newline and
 * the last row stays free for it, since a scroll would move the picture.
 * `last` is the picture's last row.
 *
 * The console has 24 rows or more, and a report at most seven: three for a
 * record, three for `str`, which a record cuts too, and one for the reset.
 */
static void place_report(const char *str, unsigned int last)
{
    size_t reason = sizeof("panic: stopped: ") - 1 + k_strlen(str);
    uint32_t cause = klog_crit_len();
    unsigned int rows;

    if (reason > KLOG_RECORD_MAX) {
        reason = KLOG_RECORD_MAX;
    }
    rows = text_rows(reason);
    if (cause != 0) {
        rows += text_rows(cause);
    }
    if (panic_timeout > 0) {
        rows++;
    }
    x = 0;
    y = last - 1;
    if (y + rows >= textcon_rows) {
        y = textcon_rows - 1 - rows;
    }
}

/**
 * The picture is drawn before the record because drawing clears the screen,
 * and a record printed first would be wiped by it; the record before the panic
 * that says why was wiped too, so `klog_show_crit` shows it again above the
 * panic's own, on the screen alone. The report reads no process: the running
 * one may be half torn down, with its pages unmapped, and its name is a string
 * a user set, which may hold an escape sequence that clears the screen.
 * Everything that can fail comes after: the record reaches the ring
 * before it is offered to the serial port and the wire, so a port that never
 * drains or a controller that never came up cannot take the record with it.
 * Both of those paths are bounded, which is why a panic can afford to use them
 * at all. The other processors are stopped first, with interrupts off here, so
 * that nothing changes the screen or the state the record describes
 * (`k_panic_begin`). Nothing after the record runs to write out klog's queue,
 * so the record goes out directly (`klog_deliver_directly`).
 *
 * The wait before a reset is Linux's loop of PANIC_TIMER_STEP milliseconds
 * (kernel/panic.c, `panic`), measured by `k_udelay`, which needs no
 * interrupt, since none is taken now; the reset is `emergency_restart`'s,
 * with nothing shut down (`power_emergency_restart`). The value is read once,
 * here, so that a write to /proc/sys/kernel/panic racing the panic gives one
 * answer or the other.
 */
void k_panic(const char *str)
{
    int timeout = panic_timeout;
    int seconds, step;

    k_panic_begin();
    k_terminal_restart();
    place_report(str, draw_fail_image());
    set_back_color(black);
    set_fore_color(white);

    klog_deliver_directly();
    klog_show_crit();
    klog(KLOG_CRIT, "panic", "stopped: %s", str);

    if (timeout > 0) {
        klog(KLOG_CRIT, "panic", "rebooting in %d seconds..", timeout);
        for (seconds = 0; seconds < timeout; seconds++) {
            for (step = 0; step < 1000; step += PANIC_TIMER_STEP) {
                k_udelay(PANIC_TIMER_STEP * 1000u);
            }
        }
    }
    if (timeout != 0) {
        power_emergency_restart();
    }
    k_stop();
}
