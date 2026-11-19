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
#include "lib/lockdep.h"
#include "lib/kstring.h"
#include "network/netconsole.h"
#include "platform.h"
#include "power.h"
#include "proc/percpu.h"
#include "proc/process.h"
#include "proc/smp.h"
#include "udelay.h"

/* The commit the kernel was built from, which the host build puts in
 * head.h (rach2/Makefile); a build from no repository, such as the guest's,
 * says so. */
#ifdef RACH2_HEAD_H
#include RACH2_HEAD_H
#endif
#ifndef RACH2_HEAD
#define RACH2_HEAD "unknown"
#endif

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

/* The first row of the picture with a cell of the character's; the rows
 * above it are background alone, and may leave the screen. */
#define FAIL_FIRST_ROW 2

/* Halt this processor for good, with interrupts off (loader.s). */
extern void k_stop(void) K_NORETURN;

/* The step of a panic's wait before its reset, in milliseconds, as Linux's
 * PANIC_TIMER_STEP (kernel/panic.c). */
#define PANIC_TIMER_STEP 100

int panic_timeout = PANIC_TIMEOUT_DEFAULT;
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
    int nth, given = 0;

    for (nth = 0; nth < kcmdline_count("panic"); nth++) {
        if (!kcmdline_value_nth("panic", nth, value, (unsigned int)sizeof(value)) ||
            k_strtoll(value, k_strlen(value), &v) != 0 || v < -0x7FFFFFFFL - 1 || v > 0x7FFFFFFFL) {
            klog(KLOG_WARN, "panic", "panic=: refused: not an integer");
            continue;
        }
        panic_timeout = (int)v;
        given = 1;
    }
    if (kcmdline_has("oops=panic")) {
        panic_on_oops = 1;
    }
    if (given) {
        klog(KLOG_INFO, "panic", "timeout: %d seconds", panic_timeout);
    }
}

/* The first processor to panic, as its number plus one, 0 before any has;
 * and whether it has stopped the others. */
static volatile unsigned int panic_owner;
static volatile int others_stopped;

void (*k_panic_hook)(void);
int (*k_panic_dump)(void);

/* The exception a panic is for, which `k_panic_note_fault` and
 * `k_panic_note_address` set before the panic. */
struct fault_note {
    const char *name;  /* NULL for none */
    uintptr_t ip;      /* the instruction it was raised at */
    long error;        /* its error code, or PANIC_NO_ERROR */
    uintptr_t address; /* what it was about, with `has_address` */
    int has_address;
};

/*
 * Each processor's note, by `arch_cpu_id()`. The handlers write it before
 * `k_panic`, and so before `k_panic_begin` has chosen the owner, and a
 * processor that faults after the first panics must not replace the
 * owner's, as Linux prints a fault from its own registers under `die_lock`
 * (arch/x86/kernel/dumpstack.c:351-371, 460-467).
 */
static struct fault_note fault_notes[NR_CPUS];

void k_panic_note_fault(const char *name, uintptr_t ip, long error)
{
    struct fault_note *n = &fault_notes[arch_cpu_id()];

    n->name = name;
    n->ip = ip;
    n->error = error;
    n->has_address = 0;
}

void k_panic_note_address(uintptr_t address)
{
    struct fault_note *n = &fault_notes[arch_cpu_id()];

    n->address = address;
    n->has_address = 1;
}

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
 *
 * The lock checker of the debug kernel stops first, as Linux's
 * `debug_locks_off`, since a hook that the report's trylocks reach would
 * wait for the checker's lock, which a stopped processor may hold.
 *
 * The watchdog's counter is stopped next, on the owner as on any other
 * that panics, as Linux's `native_stop_other_cpus` masks the caller's
 * counter entry (arch/x86/kernel/smp.c:232-235): the panicked processor
 * takes no ticks, and the counter's next NMI would report it as a hard
 * lockup, and with `nmi_watchdog=panic` panic it again under another
 * reason.
 */
void k_panic_begin(void)
{
    unsigned int me = arch_cpu_id() + 1u;
    unsigned int owner;

    arch_irq_disable();
    lockdep_off_for_panic();
    platform_pmu_disable();
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

/* The report's margin from the screen's left edge, and the blank cells it
 * keeps between its lines and the character, in cells. */
#define REPORT_MARGIN 2u
#define REPORT_GAP    1u

/* The most lines the report has, and the longest line it keeps. */
#define REPORT_LINES 9u
#define REPORT_COLS  78u

/* The report's colors: white on the picture's background. */
#define REPORT_ATTR ((uint8_t)((blue << 4) | white))

/**
 * The short report a panic shows under its picture: a few lines that a
 * person can read off a photograph and type. `text` holds `count` lines
 * from the top, each ending in a NUL, the first of which counts down to the
 * reset. `left` and `top` are where the first line is drawn, and
 * `picture_top` the screen row of the picture's row 0, below 0 when its
 * first rows are off the screen.
 */
static struct {
    char text[REPORT_LINES][REPORT_COLS + 1];
    unsigned int count;
    unsigned int left, top;
    unsigned int picture_left;
    int picture_top;
} report;

/* What the panic reads, while it reads it, that a fault may be about: the
 * running process's record for the report (READ_PROCESS), or the panicking
 * thread's stack, found through that record, for the call trace
 * (READ_STACK); 0 otherwise. A fault there panics again, and that panic and
 * any after it leave out what faulted (`process_unsafe`, `stack_unsafe`). */
#define READ_PROCESS 1
#define READ_STACK   2
static volatile int panic_reading;
static int process_unsafe, stack_unsafe;

/* The reason and the note of the owner's first panic, which a panic that a
 * fault in the read of the process raises reports again in place of its
 * own. */
static const char *panic_reason;
static struct fault_note panic_fault;

int k_panic_reading(void)
{
    return panic_reading != 0 && k_panicking();
}

/** Begin a new line of the report, empty, and return it; the last line is
 * reused once all are taken. */
static char *report_line(void)
{
    if (report.count < REPORT_LINES) {
        report.count++;
    }
    report.text[report.count - 1][0] = '\0';
    return report.text[report.count - 1];
}

/** Append `s` to `line`, as far as the line holds. */
static void put_str(char *line, const char *s)
{
    size_t n = k_strlen(line);

    while (*s != '\0' && n < REPORT_COLS) {
        line[n++] = *s++;
    }
    line[n] = '\0';
}

/** Append `v` to `line` in base `radix`, 'd' or 'x', with `0x` before a hex
 * number. */
static void put_num(char *line, long v, int radix)
{
    char digits[24];

    if (radix == 'x') {
        put_str(line, "0x");
    }
    if (k_ltoa(v, digits, sizeof(digits), radix, false) == 0) {
        digits[0] = '\0';
    }
    put_str(line, digits);
}

/** Append an address to `line`, with as many hex digits as an address has,
 * so that the addresses of a report line up and read alike. */
static void put_address(char *line, uintptr_t v)
{
    char digits[2 * sizeof(uintptr_t) + 1];
    int i;

    for (i = (int)(2 * sizeof(uintptr_t)) - 1; i >= 0; i--) {
        digits[i] = "0123456789abcdef"[v & 0xFu];
        v >>= 4;
    }
    digits[2 * sizeof(uintptr_t)] = '\0';
    put_str(line, "0x");
    put_str(line, digits);
}

/**
 * Append the running process's name to `line`, from `comm`, with each byte
 * outside printable ASCII shown as `?`: the name is a string a user set, and
 * an escape sequence in it would reach the screen and a serial terminal.
 */
static void put_comm(char *line, const char *comm)
{
    char name[sizeof(((process_t *)0)->comm)];
    size_t i;

    for (i = 0; i < sizeof(name) - 1 && comm[i] != '\0'; i++) {
        name[i] = comm[i] >= 0x20 && comm[i] < 0x7F ? comm[i] : '?';
    }
    name[i] = '\0';
    put_str(line, name);
}

/** The line that says what becomes of the machine, `seconds` before a
 * reset: the countdown, or what a timeout of 0 or below does. */
static void put_end(char *line, int timeout, int seconds)
{
    line[0] = '\0';
    if (timeout > 0) {
        put_str(line, "rebooting in ");
        put_num(line, seconds, 'd');
        put_str(line, " s");
    } else if (timeout < 0) {
        put_str(line, "rebooting");
    } else {
        put_str(line, "halted");
    }
}

/**
 * Write the report's lines, from the most general at the top to the reason
 * at the bottom, as Linux's panic ends with its reason: what becomes of the
 * machine, whether the crash record was saved, the build, the processor and
 * the process, with its name when it has one, the exception and its
 * addresses, and the reason. The lines
 * are short, so that each fits in the room beside the character (see
 * `place_report`), and a long reason is cut on the screen alone.
 *
 * The process is read only when it is safe to: `current_proc` is the
 * kernel's own record, and its pid and name are plain fields of it, but the
 * record may be what the fault is about. A fault while it is read panics
 * again, at once (`k_panic_reading`), and the second report leaves the
 * process out. The exception is the owner's own (`panic_fault`), whatever
 * another processor noted since.
 */
static void build_report(const char *str, int timeout, int saved)
{
    process_t *p;
    char *line;

    report.count = 0;
    put_end(report_line(), timeout, timeout);
    if (saved) {
        put_str(report_line(), "crash record saved");
    }
    line = report_line();
    put_str(line, "build ");
    put_str(line, RACH2_HEAD);

    line = report_line();
    put_str(line, "cpu ");
    put_num(line, (long)arch_cpu_id(), 'd');
    if (!process_unsafe) {
        panic_reading = READ_PROCESS;
        k_barrier();
        p = current_proc;
        if (p != NULL) {
            put_str(line, "  pid ");
            put_num(line, (long)p->pid, 'd');
            if (p->comm[0] != '\0') {
                line = report_line();
                put_str(line, "cmd ");
                put_comm(line, p->comm);
            }
        }
        k_barrier();
        panic_reading = 0;
    }

    if (panic_fault.name != NULL) {
        line = report_line();
        put_str(line, "ip ");
        put_address(line, panic_fault.ip);
        if (panic_fault.has_address) {
            line = report_line();
            put_str(line, "addr ");
            put_address(line, panic_fault.address);
        }
        line = report_line();
        put_str(line, panic_fault.name);
        if (panic_fault.error != PANIC_NO_ERROR) {
            put_str(line, ", error ");
            put_num(line, panic_fault.error, 'x');
        }
    }

    line = report_line();
    put_str(line, "panic: ");
    put_str(line, str);
}

/** The first column of row `row` of the picture with a cell of the
 * character's, or FAIL_COLS when the row has none. */
static unsigned int first_cell(unsigned int row)
{
    unsigned int col;

    for (col = 0; col < FAIL_COLS; col++) {
        if (_fail_image[row * FAIL_COLS + col] != blue) {
            return col;
        }
    }
    return FAIL_COLS;
}

/**
 * The cells a line of the report may take from column `left` of screen row
 * `row`: up to the right margin, and, on a row of the picture, up to
 * REPORT_GAP cells before the character's first cell there.
 */
static unsigned int room(unsigned int row, unsigned int left)
{
    unsigned int end = textcon_cols - REPORT_MARGIN;
    int r = (int)row - report.picture_top;

    if (r >= 0 && r < (int)FAIL_ROWS && first_cell((unsigned int)r) < FAIL_COLS) {
        unsigned int c = report.picture_left + first_cell((unsigned int)r);

        c = c > REPORT_GAP ? c - REPORT_GAP : 0;
        if (c < end) {
            end = c;
        }
    }
    return end > left ? end - left : 0;
}

/** Whether every line of the report fits beside the character, drawn from
 * `report.left` and `report.top`. */
static int report_fits(void)
{
    unsigned int i;

    for (i = 0; i < report.count; i++) {
        if (k_strlen(report.text[i]) > room(report.top + i, report.left)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Choose where the picture and the report go. The picture is centered. The
 * report goes below it, a row apart, when the rows allow; else over the
 * picture's bottom-left corner, REPORT_MARGIN columns from the screen's left
 * edge and a row above its bottom, with no line reaching within REPORT_GAP
 * cells of the character. When it does not fit there, the picture moves up,
 * a row at a time, as far as its blank first rows allow, which on 80 by 25
 * and on 85 by 24 gives the rows below her the whole width of the screen;
 * and when it still does not fit, the report takes the screen's last row
 * too, which 80 by 24 needs for an x86-64 kernel's addresses. A line that
 * fits nowhere is cut where the character begins, so that no cell of hers
 * is covered; its whole text is in the record.
 */
static void place_report(void)
{
    int top, lowest, bottom;
    unsigned int last;

    report.picture_left = textcon_cols > FAIL_COLS ? (textcon_cols - FAIL_COLS) / 2 : 0;
    top = textcon_rows >= FAIL_ROWS ? (int)(textcon_rows - FAIL_ROWS) / 2
                                    : -(int)(FAIL_ROWS - textcon_rows);
    report.left = report.picture_left > REPORT_MARGIN ? report.picture_left : REPORT_MARGIN;
    report.picture_top = top;

    last = (unsigned int)(top + (int)FAIL_ROWS - 1);
    if (textcon_rows - 1 - last >= report.count + 1) {
        report.top = last + 2;
        return;
    }
    lowest = -FAIL_FIRST_ROW;
    for (bottom = 1; bottom >= 0; bottom--) {
        report.top = textcon_rows - (unsigned int)bottom - report.count;
        for (report.picture_top = top; report.picture_top >= lowest; report.picture_top--) {
            if (report_fits()) {
                return;
            }
        }
    }
    report.picture_top = lowest;
}

/** Clear the screen to the picture's background and draw the picture with
 * its row 0 on screen row `report.picture_top`, leaving out the rows off
 * the screen. */
static void draw_fail_image(void)
{
    unsigned int col, row;
    int r;

    set_back_color(blue);
    k_cls();
    for (row = 0; row < FAIL_ROWS; ++row) {
        r = report.picture_top + (int)row;
        if (r < 0 || r >= (int)textcon_rows) {
            continue;
        }
        for (col = 0; col < FAIL_COLS; ++col) {
            textcon_write_cell(report.picture_left + col, (unsigned int)r, ' ',
                               (uint8_t)(_fail_image[row * FAIL_COLS + col] << 4));
        }
    }
}

/** Draw line `i` of the report, cut to its room, and blank the cells to its
 * old length `was`, which a shorter countdown leaves behind. */
static void draw_line(unsigned int i, unsigned int was)
{
    unsigned int row = report.top + i;
    unsigned int n = (unsigned int)k_strlen(report.text[i]);
    unsigned int fit = room(row, report.left);
    unsigned int col;

    if (n > fit) {
        n = fit;
    }
    for (col = 0; col < n; col++) {
        textcon_write_cell(report.left + col, row, report.text[i][col], REPORT_ATTR);
    }
    for (; col < was && col < fit; col++) {
        textcon_write_cell(report.left + col, row, ' ', REPORT_ATTR);
    }
}

/** Draw the picture and the report where `place_report` put them, and leave
 * the cursor after the last line. */
static void draw_report(void)
{
    unsigned int i;

    draw_fail_image();
    for (i = 0; i < report.count; i++) {
        draw_line(i, 0);
    }
    i = report.count - 1;
    x = report.left + (unsigned int)k_strlen(report.text[i]);
    if (x > report.left + room(report.top + i, report.left)) {
        x = report.left + room(report.top + i, report.left);
    }
    y = report.top + i;
    textcon_place_cursor();
}

/**
 * The full record goes out first, to the ring, the serial port and
 * netconsole: the reason, the return addresses of the panicking thread's
 * frames, as Linux's `panic` calls `dump_stack` (kernel/panic.c), which
 * `addr2line -e` of the kernel's image names (`platform_show_stack`), and
 * the report's lines; then the crash record is saved (`k_panic_dump`); the
 * screen takes neither (`klog_screen_stop`), and shows instead the picture
 * and the short report under it (`build_report`), which is drawn cell by
 * cell, so that nothing scrolls and nothing the log holds, such as an
 * escape sequence in a process's name, reaches the screen. The record
 * reaches the ring before it is offered to the serial port and the wire, so
 * a port that never drains or a controller that never came up cannot take
 * the record with it. Both of those paths are bounded, which is why a panic
 * can afford to use them at all. The other processors are stopped first,
 * with interrupts off here, so that nothing changes the screen or the state
 * the record describes (`k_panic_begin`). Nothing after the record runs to
 * write out klog's queue, so the record goes out directly
 * (`klog_deliver_directly`), and netconsole forgets a send the panic stopped
 * (`netconsole_panic`). The crash record, ramoops's when the line gives
 * a region, is saved before the wait, as Linux's kmsg_dump comes before its
 * wait (kernel/panic.c:440).
 *
 * The wait before a reset is Linux's loop of PANIC_TIMER_STEP milliseconds
 * (kernel/panic.c, `panic`), measured by `k_udelay`, which needs no
 * interrupt, since none is taken now; the report's countdown changes once a
 * second of it. A count too long for the room its row has, which only a
 * `panic_timeout` of many digits and a screen with no placement for the
 * whole report give (`place_report`), is not shown cut: the line says
 * "rebooting" alone, and the record has the number whole, as Linux's
 * "Rebooting in %d seconds.." (kernel/panic.c:477). The reset is
 * `emergency_restart`'s, with nothing shut down
 * (`power_emergency_restart`). The value is read once, here, once the
 * other processors have stopped, as Linux reads it after
 * `panic_other_cpus_shutdown`, and passed down, so that the report's rows,
 * its countdown and the reset all follow the one value even when a write
 * to /proc/sys/kernel/panic raced the panic.
 *
 * The walk of the stack and the report's read of the process both read
 * through `current_proc`, which may be what the fault is about, and the walk
 * reads frames a damaged stack may point anywhere; each runs with
 * `panic_reading` saying so, and a fault in it panics again at once
 * (`k_panic_reading`), before the exception router takes the big lock. A
 * panic raised so writes its own reason to the record, skips what faulted,
 * here and in any panic after it, and reports the first panic's reason and
 * exception again, since the first is what stopped the machine; Linux lets
 * the owner's nested oops go on in the same way
 * (arch/x86/kernel/dumpstack.c:361-366). A bad process pointer faults in
 * both, so that the report comes from the third panic, with neither.
 */
void k_panic(const char *str)
{
    int timeout, seconds, step, saved, counting;
    unsigned int was;

    k_panic_begin();
    timeout = *(volatile int *)&panic_timeout;
    if (panic_reading != 0) {
        if (panic_reading == READ_STACK) {
            stack_unsafe = 1;
        } else {
            process_unsafe = 1;
        }
        panic_reading = 0;
    } else {
        panic_reason = str;
        k_memcpy(&panic_fault, &fault_notes[arch_cpu_id()], sizeof(panic_fault));
    }
    k_terminal_restart();
    klog_screen_stop();
    klog_deliver_directly();
    netconsole_panic();
    klog(KLOG_CRIT, "panic", "stopped: %s", str);
    if (!stack_unsafe) {
        panic_reading = READ_STACK;
        k_barrier();
        platform_show_stack("panic", KLOG_CRIT);
        k_barrier();
        panic_reading = 0;
    }
    str = panic_reason;

    build_report(str, timeout, 0);
    for (step = 1; step < (int)report.count - 1; step++) {
        klog(KLOG_CRIT, "panic", "report: %s", report.text[step]);
    }
    if (timeout > 0) {
        klog(KLOG_CRIT, "panic", "rebooting in %d seconds..", timeout);
    }
    saved = k_panic_dump != NULL && k_panic_dump();
    if (saved) {
        build_report(str, timeout, 1);
    }
    place_report();
    counting = timeout > 0 && k_strlen(report.text[0]) <= room(report.top, report.left);
    if (timeout > 0 && !counting) {
        put_end(report.text[0], -1, 0);
    }
    draw_report();

    if (timeout > 0) {
        for (seconds = timeout; seconds > 0; seconds--) {
            for (step = 0; step < 1000; step += PANIC_TIMER_STEP) {
                k_udelay(PANIC_TIMER_STEP * 1000u);
            }
            if (counting) {
                was = (unsigned int)k_strlen(report.text[0]);
                put_end(report.text[0], timeout, seconds - 1);
                draw_line(0, was);
            }
        }
    }
    if (timeout != 0) {
        power_emergency_restart();
    }
    k_stop();
}
