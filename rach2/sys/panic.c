/*
 * panic.c: the kernel's stop on an error it cannot recover from, with the
 * picture it draws and the record that says why.
 */
#include "panic.h"

#include "arch/arch.h"
#include "klog.h"
#include "proc/smp.h"

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

/* The first processor to panic, as its number plus one, 0 before any has;
 * and whether it has stopped the others. */
static volatile unsigned int panic_owner;
static volatile int others_stopped;

/**
 * Only the first processor to panic reports. A second, which an NMI sent to
 * every processor at once can make, or a panic the stop request did not
 * reach in time, stops without a word, as Linux's does: two records would
 * be printed over each other, and neither holds a lock. The one that
 * reports writes the console without its lock (`console_lock`), which the
 * code it interrupted may hold, or be between taking and marking as its
 * own.
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
    owner = arch_atomic_cmpxchg(&panic_owner, 0u, me);
    if (owner != 0u && owner != me) {
        k_stop();
    }
    if (!others_stopped) {
        smp_stop_others();
        others_stopped = 1;
    }
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

/**
 * The picture is drawn before the record because drawing clears the screen,
 * and a record printed first would be wiped by it. Everything that can fail
 * comes after: the record reaches the ring before it is offered to the serial
 * port and the wire, so a port that never drains or a controller that never
 * came up cannot take the record with it. Both of those paths are bounded,
 * which is why a panic can afford to use them at all. The other processors
 * are stopped first, with interrupts off here, so that nothing changes the
 * screen or the state the record describes (`k_panic_begin`). Nothing after
 * the record runs to write out klog's queue, so the record goes out directly
 * (`klog_deliver_directly`).
 */
void k_panic(const char *str)
{
    k_panic_begin();
    y = draw_fail_image() - 1;
    set_back_color(black);
    set_fore_color(white);

    klog_deliver_directly();
    klog(KLOG_CRIT, "panic", "stopped: %s", str);

    k_stop();
}
