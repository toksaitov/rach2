/**
 * Host test of the lock-order checker, rach2/sys/lib/lockdep.c, through the
 * spinlock calls of rach2/sys/lib/spinlock.c.
 *
 * host-cflags: -DRACH2_LOCKDEP
 *
 * Both files are included whole, built with RACH2_LOCKDEP, and run as
 * processor 0 with no process. The test models the interrupt flag, whose
 * `kirq_restore` tells the checker when interrupts come on as the
 * kernel's does, and the interrupt depth `in_interrupt` reads; klog's
 * report path collects each record's text, which the rows read.
 *
 * The rows: two locks taken in both orders, reported once, with the order
 * taken first; a cycle of three locks, with the two orders before it; a
 * lock taken in an interrupt and elsewhere with interrupts on, by the take
 * and by interrupts coming on while it is held; an order from a lock taken
 * in an interrupt to one held with interrupts on; a sleep and a sleeping
 * lock under a spinlock, and a sleep with interrupts off; the big lock
 * under a spinlock; two locks of one class taken at once, and by
 * subclass in the right order and in the wrong one; a trylock, which adds
 * no order; a lock never initialised, a release of a lock not held and a
 * failed assertion; /proc/lockdep's and /proc/lockdep_stats's text; and
 * last, the class pool filled, which stops the checker with one report.
 */

#include <setjmp.h>

#include "lib/lockdep.c"
#include "lib/spinlock.c"

extern int printf(const char *format, ...);
extern int vsnprintf(char *buf, unsigned long size, const char *format, va_list ap);
extern char *strstr(const char *haystack, const char *needle);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("lockdep-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- what lockdep.c and spinlock.c reach outside themselves ---- */

struct percpu cpu_table[NR_CPUS];
char _data[1], _image_end[1];

static int irqs_on;    /* the modelled interrupt flag */
static int irq_depth;  /* the modelled interrupt depth */
static int serialized; /* what smp_serialized answers */
static jmp_buf panic_at;

/* Every record the checker made since the row began, one per line. */
static char out[16384];
static unsigned long out_len;
static int records;

/** The flag as EFLAGS.IF, then interrupts off. */
uint32_t kirq_save(void)
{
    uint32_t flags = irqs_on ? 0x200u : 0u;

    irqs_on = 0;
    return flags;
}

/** Put the flag back, telling the checker first when interrupts come on,
 * as arch/x86/kirq.c does. */
void kirq_restore(uint32_t flags)
{
    if ((flags & 0x200u) != 0 && !irqs_on) {
        lockdep_irqs_on(0);
    }
    irqs_on = (flags & 0x200u) != 0;
}

/** The modelled flag. */
int kirq_enabled(void)
{
    return irqs_on;
}

/** The modelled interrupt depth. */
int in_interrupt(void)
{
    return irq_depth > 0;
}

/** One processor: nothing to answer. */
void smp_service(void)
{}

/** Nothing to wait for. */
void arch_relax(void)
{}

/** The exchange, on one processor. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/** Whether the test holds the modelled big lock. */
int smp_serialized(void)
{
    return serialized;
}

/** Return to the row's setjmp. */
void k_panic(const char *str)
{
    (void)str;
    longjmp(panic_at, 1);
}

/** Collect the report under the lock the test does not model. */
int klog_report_begin(void)
{
    return KLOG_REPORT_LOCKED;
}

/** Append one record's text and a newline to `out`. */
void klog_report(int how, int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    int n;

    (void)how;
    (void)level;
    (void)subsystem;
    va_start(ap, format);
    n = vsnprintf(out + out_len, sizeof(out) - out_len - 1, format, ap);
    va_end(ap);
    if (n > 0 && out_len + (unsigned long)n + 1 < sizeof(out)) {
        out_len += (unsigned long)n;
        out[out_len++] = '\n';
        out[out_len] = '\0';
    }
    records++;
}

/** End the report. */
void klog_report_end(int how)
{
    (void)how;
}

/* ---- the rows ---- */

/** Forget the reports of the row before. */
static void row_start(void)
{
    out_len = 0;
    out[0] = '\0';
    records = 0;
}

/** Whether a record of the row has `text`. */
static int said(const char *text)
{
    return strstr(out, text) != 0;
}

/** How many times `text` is in the row's records. */
static int said_times(const char *text)
{
    const char *p = out;
    int n = 0;

    while ((p = strstr(p, text)) != 0) {
        n++;
        p++;
    }
    return n;
}

static struct spinlock lock_a = SPINLOCK_INIT_NAMED("lock_a");
static struct spinlock lock_b = SPINLOCK_INIT_NAMED("lock_b");
static struct spinlock lock_x = SPINLOCK_INIT_NAMED("lock_x");
static struct spinlock lock_y = SPINLOCK_INIT_NAMED("lock_y");
static struct spinlock lock_z = SPINLOCK_INIT_NAMED("lock_z");
static struct spinlock lock_i = SPINLOCK_INIT_NAMED("lock_i");
static struct spinlock lock_p = SPINLOCK_INIT_NAMED("lock_p");
static struct spinlock lock_q = SPINLOCK_INIT_NAMED("lock_q");
static struct spinlock lock_r = SPINLOCK_INIT_NAMED("lock_r");
static struct spinlock lock_s = SPINLOCK_INIT_NAMED("lock_s");
static struct spinlock lock_t1 = SPINLOCK_INIT_NAMED("lock_t1");
static struct spinlock lock_t2 = SPINLOCK_INIT_NAMED("lock_t2");
static struct lockdep_key mutex_key;
static struct lockdep_map mutex_m;

/** Take `first` and then `second`, and let both go. */
static void nest(struct spinlock *first, struct spinlock *second)
{
    spin_lock(first);
    spin_lock(second);
    spin_unlock(second);
    spin_unlock(first);
}

/** Two locks in both orders: one report, which names the order before. */
static void row_ab_ba(void)
{
    row_start();
    nest(&lock_a, &lock_b);
    check(records == 0, "a first order of two locks is no report");
    nest(&lock_b, &lock_a);
    check(said("possible deadlock: lock_a at ") && said(" under lock_b at ") &&
              said(", against the order before, cpu 0 pid 0"),
          "the other order of two locks is a possible deadlock");
    check(said("order: lock_a at ") && said(", then lock_b at "),
          "the report names the order taken first, with its callers");
    check(said("held: lock_b at ") && said("held: lock_a at "),
          "the report names the locks held, with their callers");
    row_start();
    nest(&lock_b, &lock_a);
    nest(&lock_a, &lock_b);
    check(records == 0, "the same pair is reported once");
}

/** x before y, y before z, and then z before x. */
static void row_three(void)
{
    row_start();
    nest(&lock_x, &lock_y);
    nest(&lock_y, &lock_z);
    check(records == 0, "a chain of three locks is no report");
    nest(&lock_z, &lock_x);
    check(said("possible deadlock: lock_x at ") && said(" under lock_z at "),
          "a third order that closes a cycle of three is a possible deadlock");
    check(said("order: lock_x at ") && said(", then lock_y at ") && said("order: lock_y at ") &&
              said(", then lock_z at "),
          "the report names both orders of the cycle");
}

/** A lock taken in an interrupt, then with interrupts on; another held
 * when interrupts come on. */
static void row_irq(void)
{
    uint32_t flags;

    row_start();
    irq_depth = 1;
    spin_lock(&lock_i);
    spin_unlock(&lock_i);
    irq_depth = 0;
    check(records == 0, "a lock taken in an interrupt alone is no report");
    irqs_on = 1;
    spin_lock(&lock_i);
    spin_unlock(&lock_i);
    irqs_on = 0;
    check(said("lock taken in an interrupt and with interrupts on: lock_i in an interrupt at ") &&
              said(", with interrupts on at "),
          "a lock taken in an interrupt and with interrupts on is reported");

    row_start();
    irq_depth = 1;
    spin_lock(&lock_r);
    spin_unlock(&lock_r);
    irq_depth = 0;
    flags = spin_lock_irqsave(&lock_s);
    spin_lock(&lock_r);
    irqs_on = 1;
    lockdep_irqs_on(0);
    irqs_on = 0;
    spin_unlock(&lock_r);
    spin_unlock_irqrestore(&lock_s, flags);
    check(said("lock taken in an interrupt and with interrupts on: lock_r"),
          "interrupts coming on with a lock held mark it as held with interrupts on");
}

/** p is taken in an interrupt, q with interrupts on, then p before q. */
static void row_inversion(void)
{
    row_start();
    irq_depth = 1;
    spin_lock(&lock_p);
    spin_unlock(&lock_p);
    irq_depth = 0;
    irqs_on = 1;
    spin_lock(&lock_q);
    spin_unlock(&lock_q);
    irqs_on = 0;
    check(records == 0, "two locks used apart are no report");
    nest(&lock_p, &lock_q);
    check(said("interrupt inversion: lock_p, taken in an interrupt at ") &&
              said(", is taken before lock_q, held with interrupts on at "),
          "an order from a lock taken in an interrupt to one held with interrupts on is reported");
    check(said("order: lock_p at ") && said(", then lock_q at "), "the inversion names its order");
}

/** Sleeps and a sleeping lock under a spinlock, a sleep with interrupts
 * off, and the big lock under a spinlock. */
static void row_sleep(void)
{
    row_start();
    irqs_on = 1;
    lockdep_might_sleep(0x1234u);
    lockdep_acquire(&mutex_m, 0, LOCKDEP_SLEEPING, 0x1000u);
    lockdep_release(&mutex_m, 0x1004u);
    check(records == 0, "a sleep and a sleeping lock with nothing held are no report");
    irqs_on = 0;
    spin_lock(&lock_s);
    lockdep_might_sleep(0x1234u);
    check(said("sleep at 0x1234 under spinlock lock_s at "),
          "a sleep under a spinlock is reported");
    lockdep_acquire(&mutex_m, 0, LOCKDEP_SLEEPING, 0x1000u);
    lockdep_release(&mutex_m, 0x1004u);
    check(said("sleeping lock mutex_m at 0x1000 under spinlock lock_s at "),
          "a sleeping lock under a spinlock is reported");
    lockdep_bkl_acquire(0x2000u);
    check(said("big lock at 0x2000 under spinlock lock_s at "),
          "the big lock under a spinlock is reported");
    spin_unlock(&lock_s);
    row_start();
    lockdep_might_sleep(0x3000u);
    check(said("sleep at 0x3000 with interrupts off"), "a sleep with interrupts off is reported");
    row_start();
    lockdep_might_sleep(0x3000u);
    check(records == 0, "a sleep at one site is reported once");
}

/** Two locks of one class, taken at once and by subclass. */
static void row_class(void)
{
    struct spinlock *pair = (struct spinlock *)calloc(2, sizeof(*pair));
    int i;

    for (i = 0; i < 2; i++) {
        spin_lock_init(&pair[i], "pair");
    }
    row_start();
    spin_lock(&pair[0]);
    spin_lock_nested(&pair[1], 1);
    spin_unlock(&pair[1]);
    spin_unlock(&pair[0]);
    check(records == 0, "two locks of one class taken by subclass in order are no report");
    spin_lock_nested(&pair[1], 1);
    spin_lock(&pair[0]);
    spin_unlock(&pair[0]);
    spin_unlock(&pair[1]);
    check(said("possible deadlock: pair at ") && said(" under pair/1 at "),
          "two locks of one class taken by subclass in the wrong order are a possible deadlock");
    row_start();
    spin_lock(&pair[0]);
    spin_lock(&pair[1]);
    spin_unlock(&pair[1]);
    spin_unlock(&pair[0]);
    check(said("recursive lock: pair at ") && said(", already held at "),
          "two locks of one class taken at once without a subclass are a recursive lock");
    free(pair);
}

/** A trylock under a lock adds no order. */
static void row_trylock(void)
{
    row_start();
    spin_lock(&lock_t1);
    check(spin_trylock(&lock_t2) == 1, "a trylock of a free lock takes it");
    spin_unlock(&lock_t2);
    spin_unlock(&lock_t1);
    nest(&lock_t2, &lock_t1);
    check(records == 0, "a trylock adds no order");
}

/** A lock never initialised, a release of a lock not held, and the
 * assertions. */
static void row_misuse(void)
{
    struct spinlock *bare = (struct spinlock *)calloc(1, sizeof(*bare));
    int panicked = 0;

    row_start();
    spin_lock(bare);
    spin_unlock(bare);
    check(said("lock without a class: ") && said(", keyed by its address"),
          "a lock never initialised is reported");
    row_start();
    spin_lock(bare);
    spin_unlock(bare);
    check(records == 0, "a lock never initialised is reported once");
    free(bare);

    row_start();
    lockdep_release(&lock_a.dep, 0x4000u);
    check(said("release of a lock not held: lock_a at 0x4000"),
          "a release of a lock not held is reported");

    row_start();
    spin_lock(&lock_a);
    lockdep_assert_held(&lock_a);
    spin_unlock(&lock_a);
    check(records == 0, "an assertion of a lock held is no report");
    lockdep_assert_held(&lock_a);
    check(said("lock not held: lock_a at "), "an assertion of a lock not held is reported");

    serialized = 1;
    bkl_assert_held();
    serialized = 0;
    if (setjmp(panic_at) == 0) {
        bkl_assert_held();
    } else {
        panicked = 1;
    }
    check(panicked, "an assertion of the big lock without it panics");
}

/** /proc/lockdep and /proc/lockdep_stats. */
static void row_proc(void)
{
    static char text[65536];

    lockdep_proc_text(text, (int)sizeof(text));
    check(strstr(text, "all lock classes:\n") != 0, "/proc/lockdep has its header");
    check(strstr(text, ": lock_a\n -> lock_b\n") != 0,
          "/proc/lockdep lists a class and the class taken under it");
    check(strstr(text, " +.: lock_p\n") != 0 && strstr(text, " .-: lock_q\n") != 0,
          "/proc/lockdep marks a class's use in an interrupt and with interrupts on");
    lockdep_stats_text(text, (int)sizeof(text));
    check(strstr(text, " lock-classes:") != 0 && strstr(text, "[max: 1024]") != 0,
          "/proc/lockdep_stats counts the classes against the pool");
    check(strstr(text, " checking:                      on\n") != 0,
          "/proc/lockdep_stats says the checker is on");
}

/** Fill the class pool: the checker stops with one report. */
static void row_overflow(void)
{
    static struct lockdep_key keys[LOCKDEP_CLASSES + 1];
    static char text[4096];
    struct spinlock *many = (struct spinlock *)calloc(LOCKDEP_CLASSES + 1, sizeof(*many));
    int i;

    row_start();
    for (i = 0; i <= LOCKDEP_CLASSES; i++) {
        many[i].locked = 0;
        lockdep_init_map(&many[i].dep, "many", &keys[i]);
        spin_lock(&many[i]);
        spin_unlock(&many[i]);
    }
    check(said_times("off: the class pool is full") == 1,
          "a full class pool stops the checker with one report");
    lockdep_stats_text(text, (int)sizeof(text));
    check(strstr(text, " checking:                      off: the class pool is full\n") != 0,
          "/proc/lockdep_stats says why the checker stopped");
    row_start();
    nest(&lock_b, &lock_a);
    check(records == 0, "a stopped checker reports nothing");
    check(lockdep_is_held(&lock_a.dep), "a stopped checker takes every lock as held");
    free(many);
}

/** Copy `n` bytes from `src`: the test's user memory is its own. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    memcpy(dst, (const void *)src, n);
    return 0;
}

/** Copy `n` bytes. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    return memcpy(dest, src, n);
}

/** Enter the modelled interrupt. */
void irq_enter(void)
{
    irq_depth++;
}

/** Leave the modelled interrupt. */
void irq_leave(void)
{
    irq_depth--;
}

int main(void)
{
    lockdep_init_map(&mutex_m, "mutex_m", &mutex_key);
    row_ab_ba();
    row_three();
    row_irq();
    row_inversion();
    row_sleep();
    row_class();
    row_trylock();
    row_misuse();
    row_proc();
    row_overflow();
    if (fails) {
        printf("lockdep-hosttest: %d failure(s)\n", fails);
        return 1;
    }
    printf("lockdep-hosttest: ok\n");
    return 0;
}
