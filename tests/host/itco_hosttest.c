/* Intel TCO register sequencing, two-stage expiry, watchdog file ABI, the
 * kernel's feed before a program opens the device, and the options the
 * driver reads from the command line through cmdline.c.
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include <stdarg.h>

extern int printf(const char *format, ...);
extern int strcmp(const char *a, const char *b);

#undef RACH2_HOSTTEST
#include "arch/x86/itco.c"

extern int vsnprintf(char *out, size_t size, const char *format, va_list ap);

struct model {
    uint16_t words[16];
    uint32_t gcs;
    unsigned left, stage, resets, writes;
    unsigned regs[128];
    uint32_t values[128];
    int locked, absent, stuck_timer, stuck_halt;
};

static int failures;
volatile uint64_t kernel_ticks;
static struct model model;
static const void *bad_user;
static const char *refused_keys[8]; /* the options of the first refusals */
static int refused;                 /* the `refused:` records since `setup` */
static char boot_line[128];         /* the last `boot:` record, formatted */

#define CHECK(test)                                                  \
    do {                                                             \
        if (!(test)) {                                               \
            printf("itco_hosttest: line %d: %s\n", __LINE__, #test); \
            failures++;                                              \
        }                                                            \
    } while (0)

/** Record one bounded register write; excess writes are a test failure. */
static void record(struct model *m, unsigned reg, uint32_t value)
{
    CHECK(m->writes < 128);
    if (m->writes < 128) {
        m->regs[m->writes] = reg;
        m->values[m->writes++] = value;
    }
}

/** Read a TCO register; reserved bits are kept by the model. */
static uint16_t model_rd(void *ctx, unsigned reg)
{
    struct model *m = ctx;

    CHECK(reg <= TCO_TMR && !(reg & 1));
    if (m->absent) {
        return 0xFFFFu;
    }
    return reg == TCO_RLD ? (uint16_t)m->left : m->words[reg / 2];
}

/** Implement halt, reload and write-one-to-clear status, independently of
 * the driver. Reload resets both stages; writing the initial value does not. */
static void model_wr(void *ctx, unsigned reg, uint16_t value)
{
    struct model *m = ctx;

    record(m, reg, value);
    if (reg == TCO1_STS || reg == TCO2_STS) {
        m->words[reg / 2] &= (uint16_t)~value;
    } else if (reg == TCO_RLD) {
        m->stage = 0;
        m->left = m->words[TCO_TMR / 2] & TCO_MASK;
    } else if ((reg != TCO_TMR || !m->stuck_timer) && (reg != TCO1_CNT || !m->stuck_halt)) {
        m->words[reg / 2] = value;
    }
}

/** Read GCS, or the value of an unclaimed MMIO window. */
static uint32_t model_gcs_rd(void *ctx)
{
    struct model *m = ctx;

    return m->absent ? 0xFFFFFFFFu : m->gcs;
}

/** Firmware may lock NO_REBOOT; all unrelated GCS bits remain writable. */
static void model_gcs_wr(void *ctx, uint32_t value)
{
    struct model *m = ctx;

    record(m, 0x3410, value);
    m->gcs = m->locked ? value | TCO_NO_REBOOT : value;
}

/** Advance a specified number of hardware ticks, without a host clock. */
static void advance(struct model *m, unsigned ticks)
{
    while (ticks-- && !(m->words[TCO1_CNT / 2] & TCO_HALT)) {
        if (m->left && --m->left == 0) {
            m->words[TCO1_STS / 2] |= 8;
            if (++m->stage == 2) {
                m->words[TCO2_STS / 2] |= 6;
                if (!(m->gcs & TCO_NO_REBOOT)) {
                    m->resets++;
                }
                m->stage = 0;
            }
            m->left = m->words[TCO_TMR / 2] & TCO_MASK;
        }
    }
}

/** Reset the model and bind the production watchdog to its callbacks. */
static void setup(void)
{
    unsigned i;
    char *bytes = (char *)&model;

    for (i = 0; i < sizeof(model); i++) {
        bytes[i] = 0;
    }
    model.words[TCO1_CNT / 2] = TCO_HALT | 0x100;
    model.words[TCO_TMR / 2] = 0xAC04;
    model.words[TCO1_STS / 2] = 0x108;
    model.words[TCO2_STS / 2] = 0x106;
    model.gcs = 0x12340020;
    watchdog.rd = model_rd;
    watchdog.wr = model_wr;
    watchdog.gcs_rd = model_gcs_rd;
    watchdog.gcs_wr = model_gcs_wr;
    watchdog.ctx = &model;
    watchdog.opened = watchdog.magic = watchdog.timeout = 0;
    watchdog.running = watchdog.pinged = 0;
    watchdog.hw_running = watchdog.nowayout = watchdog.open_limited = 0;
    watchdog.boot_feed = 1;
    watchdog.hz = 5;
    watchdog.feed_left = watchdog.open_left = 0;
    watchdog.threaded = watchdog.feed_due = 0;
    bad_user = NULL;
    refused = 0;
}

/** Copy valid model user bytes, or fail the designated fault address. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    size_t i;

    if (!src || src == bad_user) {
        return -EFAULT;
    }
    for (i = 0; i < n; i++) {
        ((char *)dst)[i] = ((const char *)src)[i];
    }
    return 0;
}

/** Give the copyout path the same address faults as copyin. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst || dst == bad_user) {
        return -EFAULT;
    }
    return copy_from_user(dst, src, n);
}

/** No boot probe runs in this test. */
uint16_t board_pmbase(void)
{
    return 0;
}
/** No boot probe runs in this test. */
uint32_t board_rcba(void)
{
    return 0;
}
/** No physical mapping is needed for the callback model. */
uintptr_t map_mmio(uintptr_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
    return 0;
}
/** No hardware port is reached by the callback model. */
uint16_t inw(uint16_t port)
{
    (void)port;
    return 0xFFFFu;
}
/** No hardware port is reached by the callback model. */
void outw(uint16_t port, uint16_t value)
{
    (void)port;
    (void)value;
}
/** No hardware port is reached by the callback model. */
uint32_t ind(uint16_t port)
{
    (void)port;
    return 0xFFFFFFFFu;
}
/** No hardware port is reached by the callback model. */
void outd(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}
/** The node is tested through its production operations directly. */
int32_t devfs_node_register(struct devfs_node *node)
{
    (void)node;
    return 0;
}
/** No thread is made here: the rows call `itco_feed_work` for watchdogd. */
uintptr_t kstack_alloc(void)
{
    return 0;
}
/** No thread is made here. */
void kstack_free(uintptr_t top)
{
    (void)top;
}
/** No thread is made here. */
process_t *alloc_process(void)
{
    return NULL;
}
/** No thread is made here. */
void proc_set_comm(process_t *p, const char *path)
{
    (void)p;
    (void)path;
}
/** No thread is made here. */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    (void)proc;
    (void)entry_point;
}
/** No thread is made here. */
void sched_make_ready(process_t *p)
{
    (void)p;
}
/** The rows set `threaded` themselves. */
int sched_is_enabled(void)
{
    return 0;
}
/** No interrupts here. */
uint32_t kirq_save(void)
{
    return 0;
}
/** No interrupts here. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}
/** No interrupts here. */
void arch_irq_enable(void)
{}
/** watchdogd never runs here. */
int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}
/** watchdogd never runs here. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}
/** Record the option each `refused: KEY` or `refused: KEY=VALUE` names,
 * and keep the last `boot:` record whole; other diagnostics have no side
 * effect in the model. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    static const char prefix[] = "refused: %s";
    va_list ap;
    unsigned i;

    (void)level;
    (void)subsystem;
    if (format[0] == 'b' && format[1] == 'o' && format[2] == 'o' && format[3] == 't') {
        va_start(ap, format);
        vsnprintf(boot_line, sizeof(boot_line), format, ap);
        va_end(ap);
        return;
    }
    for (i = 0; prefix[i] != '\0' && format[i] == prefix[i]; i++) {
        /* compare the format with the refusal's */
    }
    if (prefix[i] != '\0') {
        return;
    }
    va_start(ap, format);
    if (refused < (int)(sizeof(refused_keys) / sizeof(refused_keys[0]))) {
        refused_keys[refused] = va_arg(ap, const char *);
    }
    refused++;
    va_end(ap);
}

/** How many of the refusals since `setup` name `key`. */
static int refusals(const char *key)
{
    int i, n = 0;
    size_t j;

    for (i = 0; i < refused && i < (int)(sizeof(refused_keys) / sizeof(refused_keys[0])); i++) {
        for (j = 0; key[j] != '\0' && refused_keys[i][j] == key[j]; j++) {
            /* compare the refused option's name with `key` */
        }
        n += key[j] == '\0' && refused_keys[i][j] == '\0';
    }
    return n;
}

/** GETSTATUS reports a pending magic close and a feed since the last call,
 * as Linux's core does; SETOPTIONS without a flag is Linux's -ENOTTY. The
 * timer was armed last and not fed since. */
static void status(void)
{
    int value = -1;

    CHECK(itco_ioctl(NULL, 0x80045701u, (uintptr_t)&value) == 0 && value == 0x8000);
    CHECK(itco_ioctl(NULL, 0x80045701u, (uintptr_t)&value) == 0 && value == 0);
    CHECK(itco_write(NULL, "V", 1) == 1);
    CHECK(itco_ioctl(NULL, 0x80045701u, (uintptr_t)&value) == 0 && value == 0x8100);
    CHECK(itco_write(NULL, "x", 1) == 1);
    CHECK(itco_ioctl(NULL, 0x80045702u, (uintptr_t)&value) == 0 && value == 0);
    value = 0;
    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == -ENOTTY);
}

/** A timer halted by SETOPTIONS is not written by a feed, a second halt or
 * a close without magic, as Linux's core skips an inactive device. */
static void halted(void)
{
    int value = 1;
    unsigned at;

    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == 0 && !watchdog.running);
    at = model.writes;
    CHECK(itco_ioctl(NULL, 0x80045705u, 0) == 0 && itco_write(NULL, "x", 1) == 1);
    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == 0);
    itco_close(NULL);
    CHECK(model.writes == at && (model.words[TCO1_CNT / 2] & TCO_HALT));
}

/** The Lenovo trial's path, start to finish: attach, open, a 30-second
 * timeout, two minutes fed every 10 s with the time left read, then no feed
 * until both stages expire 60 s after the last one. Only TCO_RLD is written
 * between the timeout and the reset. */
static void trial(void)
{
    int value = 30, left = -1;
    unsigned i, at;

    setup();
    CHECK(itco_attach(&watchdog, 30) == 0 && (model.gcs & TCO_NO_REBOOT));
    CHECK(itco_open(NULL, NULL, 0) == 0 && !(model.gcs & TCO_NO_REBOOT));
    CHECK(itco_ioctl(NULL, 0xC0045706u, (uintptr_t)&value) == 0 && value == 30);
    CHECK((model.words[TCO_TMR / 2] & TCO_MASK) == 50);
    at = model.writes;
    for (i = 0; i < 12; i++) {
        advance(&model, 16);
        CHECK(itco_ioctl(NULL, 0x8004570Au, (uintptr_t)&left) == 0 && left == 20);
        CHECK(itco_write(NULL, "x", 1) == 1 && model.left == 50);
    }
    CHECK(model.resets == 0 && model.stage == 0 && !(model.words[TCO1_STS / 2] & 8));
    advance(&model, 10);
    CHECK(itco_ioctl(NULL, 0x8004570Au, (uintptr_t)&left) == 0 && left == 24);
    advance(&model, 40);
    CHECK(model.stage == 1 && model.resets == 0 && (model.words[TCO1_STS / 2] & 8));
    CHECK(itco_ioctl(NULL, 0x8004570Au, (uintptr_t)&left) == 0 && left == 30);
    advance(&model, 49);
    CHECK(model.resets == 0);
    advance(&model, 1);
    CHECK(model.resets == 1 && (model.words[TCO2_STS / 2] & 2));
    for (i = at; i < model.writes; i++) {
        CHECK(model.regs[i] == TCO_RLD);
    }
    CHECK(model.writes - at == 12);
}

/** The attach reads TCO1_STS and TCO2_STS before it clears them, and the
 * boot record logs both and, when SECOND_TO_STS is set, that the second
 * stage expired, as after the reset the second expiry makes; the first
 * stage's TIMEOUT alone adds nothing. WDIOC_GETBOOTSTATUS stays 0, as in
 * Linux's iTCO_wdt, since the bit is set under NO_REBOOT too. */
static void boot_status(void)
{
    int value = -1;

    setup();
    model.words[TCO1_STS / 2] = 8;
    model.words[TCO2_STS / 2] = 0;
    CHECK(itco_attach(&watchdog, 3) == 0 && watchdog.boot_sts1 == 8 && watchdog.boot_sts2 == 0);
    itco_boot_log(&watchdog);
    CHECK(strcmp(boot_line, "boot: TCO1_STS 8, TCO2_STS 0") == 0);
    CHECK(itco_ioctl(NULL, 0x80045702u, (uintptr_t)&value) == 0 && value == 0);
    CHECK(itco_open(NULL, NULL, 0) == 0);
    advance(&model, 10);
    CHECK(model.resets == 1 && (model.words[TCO2_STS / 2] & 2));
    /* The reset halts the timer and starts the next boot's attach. */
    model.words[TCO1_CNT / 2] |= TCO_HALT;
    watchdog.opened = watchdog.running = 0;
    CHECK(itco_attach(&watchdog, 3) == 0 && watchdog.boot_sts2 == 6);
    itco_boot_log(&watchdog);
    CHECK(strcmp(boot_line, "boot: TCO1_STS 8, TCO2_STS 6: the TCO timer's second stage expired "
                            "before this boot") == 0);
    value = -1;
    CHECK(itco_ioctl(NULL, 0x80045702u, (uintptr_t)&value) == 0 && value == 0);
    CHECK(model.words[TCO2_STS / 2] == 0);
    CHECK(itco_attach(&watchdog, 3) == 0 && watchdog.boot_sts2 == 0);
    itco_boot_log(&watchdog);
    CHECK(strcmp(boot_line, "boot: TCO1_STS 0, TCO2_STS 0") == 0);
}

/** Run `seconds` of the kernel's tick at 5 a second, the model's 0.6 s
 * tick every third, and watchdogd after each tick when `threads` is
 * nonzero. */
static void run_threads(unsigned seconds, int threads)
{
    unsigned i;

    for (i = 1; i <= seconds * 5u; i++) {
        itco_feed_tick(&watchdog, 1);
        if (threads) {
            itco_feed_work(&watchdog);
        }
        if (i % 3u == 0) {
            advance(&model, 1);
        }
    }
}

/** Run `seconds` of the tick and of watchdogd. */
static void run(unsigned seconds)
{
    run_threads(seconds, 1);
}

/** A timer the kernel starts at boot with a 5 s heartbeat: fed by the
 * tick every 2.5 s until a program opens the device, and reset two expiries
 * after the tick stops, as after a panic; not fed with the core's
 * handle_boot_enabled off. */
static void boot(void)
{
    setup();
    CHECK(itco_attach(&watchdog, 5) == 0 && watchdog.timeout == 5 && !watchdog.hw_running);
    CHECK((model.words[TCO_TMR / 2] & TCO_MASK) == 8);
    CHECK(itco_boot_arm(&watchdog) == 0 && watchdog.hw_running && !watchdog.running);
    CHECK(!(model.gcs & TCO_NO_REBOOT) && !(model.words[TCO1_CNT / 2] & TCO_HALT));
    run(60);
    CHECK(model.resets == 0 && model.stage == 0);
    /* The tick stops: the last feed was at most 2.5 s, 5 model ticks, ago. */
    advance(&model, 10);
    CHECK(model.resets == 0);
    advance(&model, 6);
    CHECK(model.resets == 1);

    setup();
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    run(30);
    CHECK(itco_open(NULL, NULL, 0) == 0 && watchdog.running);
    run(9);
    CHECK(model.resets == 0);
    run(2);
    CHECK(model.resets == 1);
    CHECK(itco_write(NULL, "V", 1) == 1);
    itco_close(NULL);
    CHECK(!watchdog.hw_running && (model.words[TCO1_CNT / 2] & TCO_HALT));
    run(30);
    CHECK(model.resets == 1);

    setup();
    watchdog.boot_feed = 0;
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    run(10);
    CHECK(model.resets == 1);
}

/** With watchdogd running, the tick only leaves it each feed: the timer
 * lives while the thread runs, and is reset two expiries after the thread
 * stops though the tick goes on, as after a hang with interrupts on. A feed
 * left due when a program opens the device is not done. */
static void threaded(void)
{
    unsigned at;

    setup();
    watchdog.threaded = 1;
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    at = model.writes;
    itco_feed_tick(&watchdog, 1);
    CHECK(watchdog.feed_due && model.writes == at);
    itco_feed_work(&watchdog);
    CHECK(!watchdog.feed_due && model.writes == at + 1 && model.regs[at] == TCO_RLD);
    run(60);
    CHECK(model.resets == 0 && model.stage == 0);
    run_threads(5, 0);
    CHECK(model.resets == 0);
    run_threads(6, 0);
    CHECK(model.resets == 1);

    setup();
    watchdog.threaded = 1;
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    itco_feed_tick(&watchdog, 1);
    CHECK(itco_open(NULL, NULL, 0) == 0 && watchdog.feed_due);
    at = model.writes;
    itco_feed_work(&watchdog);
    CHECK(!watchdog.feed_due && model.writes == at);
}

/** The open deadline, watchdog.open_timeout: fed until a timeout before it,
 * then reset; an open before it ends it. */
static void deadline(void)
{
    setup();
    watchdog.open_limited = 1;
    watchdog.open_left = 20 * 5;
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    run(19);
    CHECK(model.resets == 0);
    run(20);
    CHECK(model.resets >= 1);

    setup();
    watchdog.open_limited = 1;
    watchdog.open_left = 20 * 5;
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    run(10);
    CHECK(itco_open(NULL, NULL, 0) == 0 && !watchdog.open_limited);
    CHECK(itco_write(NULL, "V", 1) == 1);
    itco_close(NULL);
    run(60);
    CHECK(model.resets == 0);
}

/** nowayout refuses a magic close and DISABLECARD with -EBUSY and leaves
 * the timer running, fed once at the close, as watchdog_stop and
 * watchdog_release do; a heartbeat out of range takes 30 s; a timer that
 * firmware left counting is kept running and fed, NO_REBOOT cleared if it
 * was set, as iTCO_wdt_probe does. */
static void options(void)
{
    int value = 1;

    setup();
    watchdog.nowayout = 1;
    CHECK(itco_attach(&watchdog, 30) == 0 && itco_open(NULL, NULL, 0) == 0);
    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == -EBUSY && watchdog.running);
    CHECK(itco_write(NULL, "V", 1) == 1);
    advance(&model, 10);
    itco_close(NULL);
    CHECK(watchdog.running && watchdog.hw_running && model.left == 50);
    CHECK(!(model.words[TCO1_CNT / 2] & TCO_HALT));

    setup();
    CHECK(itco_attach(&watchdog, 700) == 1 && watchdog.timeout == 30);
    CHECK(itco_attach(&watchdog, 2) == 1 && watchdog.timeout == 30);

    setup();
    model.words[TCO1_CNT / 2] = 0x100;
    model.gcs = 0x12340000;
    model.left = 3;
    CHECK(itco_attach(&watchdog, 5) == 0 && watchdog.hw_running);
    CHECK(!(model.words[TCO1_CNT / 2] & TCO_HALT) && !(model.gcs & TCO_NO_REBOOT));
    run(60);
    CHECK(model.resets == 0);

    setup();
    model.words[TCO1_CNT / 2] = 0x100;
    model.left = 3;
    CHECK(itco_attach(&watchdog, 5) == 0 && watchdog.hw_running);
    CHECK(!(model.words[TCO1_CNT / 2] & TCO_HALT) && model.gcs == 0x12340000);
    run(60);
    CHECK(model.resets == 0);
    advance(&model, 16);
    CHECK(model.resets == 1);
}

/** The parser's counts and booleans that the options below rest on: a bare
 * token counts only when it is the whole flag, with no `=`, and a boolean is
 * read from its first two bytes as Linux's kstrtobool reads it (Linux
 * 6.12.115, lib/kstrtox.c:348-388), refusing the rest and leaving the
 * value alone. */
static void parser(void)
{
    static const char *const yes[] = {"y", "Y", "t", "T", "1", "on", "ON", "oN", "yes", "1x"};
    static const char *const no[] = {"n", "N", "f", "F", "0", "off", "OFF", "oF", "no", "0x"};
    static const char *const neither[] = {"", "o", "ox", "x", "2", "-1", " y", "e", "d"};
    unsigned i;
    int v;

    CHECK(kcmdline_capture("a a=1 ab xa\ta  a=  a b=a") == 0);
    CHECK(kcmdline_bare_count("a") == 3 && kcmdline_has("a"));
    CHECK(kcmdline_bare_count("b") == 0 && !kcmdline_has("b"));
    CHECK(kcmdline_bare_count("ab") == 1 && kcmdline_bare_count("x") == 0);
    CHECK(kcmdline_count("a") == 2);
    for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        v = 0;
        CHECK(kcmdline_parse_bool(yes[i], &v) == 0 && v == 1);
    }
    for (i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        v = 1;
        CHECK(kcmdline_parse_bool(no[i], &v) == 0 && v == 0);
    }
    for (i = 0; i < sizeof(neither) / sizeof(neither[0]); i++) {
        v = 7;
        CHECK(kcmdline_parse_bool(neither[i], &v) == -1 && v == 7);
    }
    (void)kcmdline_capture(0);
}

/** The options the command line gives, as the driver reads them at boot: a
 * bare number option and a value that is no boolean are each refused with
 * a record and leave the option as it was, as Linux's parse_args refuses a
 * parameter that takes a value given none and logs each refusal (Linux
 * 6.12.115, kernel/params.c:136-138 and 203-205), and param_set_bool refuses
 * what kstrtobool does (:312-319); a bare boolean is true. */
static void line(void)
{
    int heartbeat = 0, arm = 1;

    setup();
    CHECK(kcmdline_capture("iTCO_wdt.heartbeat iTCO_wdt.nowayout=x") == 0);
    itco_options(&heartbeat, &arm);
    CHECK(heartbeat == ITCO_DEFAULT_TIMEOUT && !arm);
    CHECK(!watchdog.nowayout && watchdog.boot_feed && !watchdog.open_limited);
    CHECK(refused == 2 && refusals("iTCO_wdt.heartbeat") == 1);
    CHECK(refusals("iTCO_wdt.nowayout") == 1);

    setup();
    CHECK(kcmdline_capture("iTCO_wdt.heartbeat=10 iTCO_wdt.heartbeat iTCO_wdt.nowayout=on "
                           "iTCO_wdt.nowayout=x watchdog.handle_boot_enabled=n "
                           "rach2.itco.arm") == 0);
    itco_options(&heartbeat, &arm);
    CHECK(heartbeat == 10 && arm && watchdog.nowayout && !watchdog.boot_feed);
    CHECK(refused == 2 && refusals("iTCO_wdt.heartbeat") == 1);
    CHECK(refusals("iTCO_wdt.nowayout") == 1);

    setup();
    CHECK(kcmdline_capture("iTCO_wdt.nowayout watchdog.handle_boot_enabled=off "
                           "watchdog.open_timeout=7") == 0);
    itco_options(&heartbeat, &arm);
    CHECK(heartbeat == ITCO_DEFAULT_TIMEOUT && !arm && watchdog.nowayout && !watchdog.boot_feed);
    CHECK(watchdog.open_limited && watchdog.open_left == 7u * TICK_HZ && refused == 0);
    (void)kcmdline_capture(0);
}

/** Check elapsed countdowns, exact open expiry and projected deadlines. The
 * ordinary run ends with these rows; `--elapsed-only` runs them alone. */
static void elapsed_rows(void)
{
    unsigned before;

    setup();
    CHECK(itco_attach(&watchdog, 5) == 0 && itco_boot_arm(&watchdog) == 0);
    watchdog.boot_feed = 1;
    watchdog.hz = TICK_HZ;
    watchdog.feed_left = 10;
    watchdog.open_limited = 1;
    watchdog.open_left = 100;
    before = model.writes;
    itco_feed_tick(&watchdog, 0);
    CHECK(watchdog.feed_left == 10 && watchdog.open_left == 100 && model.writes == before);
    itco_feed_tick(&watchdog, 7);
    CHECK(watchdog.feed_left == 3 && watchdog.open_left == 93 && model.writes == before);
    itco_feed_tick(&watchdog, 3);
    CHECK(watchdog.feed_left == 0 && watchdog.open_left == 90 && model.writes == before);
    itco_feed_tick(&watchdog, 1);
    CHECK(model.writes > before && watchdog.open_left == 89);
    watchdog.feed_left = 0;
    watchdog.open_left = 5;
    before = model.writes;
    itco_feed_tick(&watchdog, 6);
    CHECK(watchdog.open_left == 0 && model.writes == before);
    registered = 1;
    watchdog.open_limited = 0;
    watchdog.feed_left = 10;
    feed_seen = 0xFFFFFFFCu;
    kernel_ticks = 0x100000000ULL;
    CHECK(itco_idle_ticks() == 7);
    kernel_ticks += 7;
    CHECK(itco_idle_ticks() == 1);
}

/** Check the attach's refusals: an absent block, a locked NO_REBOOT and a
 * timeout that does not stick; and a running timer whose halt does not
 * stick, which is attached and left counting, and whose stop fails. */
static void attach_rows(void)
{
    setup();
    model.absent = 1;
    CHECK(itco_attach(&watchdog, 30) == -EIO && model.writes == 0);
    setup();
    model.locked = 1;
    CHECK(itco_attach(&watchdog, 30) == -ENODEV);
    CHECK(model.writes == 1 && model.regs[0] == 0x3410);
    CHECK(model.words[TCO1_CNT / 2] & TCO_HALT);
    setup();
    model.stuck_timer = 1;
    CHECK(itco_attach(&watchdog, 30) == -EIO);
    setup();
    model.stuck_halt = 1;
    model.words[TCO1_CNT / 2] &= (uint16_t)~TCO_HALT;
    CHECK(itco_attach(&watchdog, 30) == 0 && watchdog.hw_running);
    CHECK(itco_stop(&watchdog) == -EIO && !watchdog.hw_running && (model.gcs & TCO_NO_REBOOT));
}

/** Check actual writes, expiry, failures, exclusive open and the ioctl ABI. */
int main(int argc, char **argv)
{
    int value, before;
    unsigned at;
    struct {
        uint32_t options, firmware;
        char identity[32];
    } info;

    if (argc == 2 && strcmp(argv[1], "--elapsed-only") == 0) {
        elapsed_rows();
        printf("itco_hosttest: elapsed: %s\n", failures ? "failed" : "ok");
        return failures != 0;
    }
    setup();
    CHECK(itco_attach(&watchdog, 30) == 0);
    CHECK(model.words[TCO1_STS / 2] == 0x100);
    CHECK(model.words[TCO2_STS / 2] == 0x100);
    CHECK(model.words[TCO_TMR / 2] == (0xAC00 | 50));
    CHECK(itco_node.major == 10 && itco_node.minor == 130 && itco_node.mode == 0600);
    at = model.writes;
    CHECK(itco_open(NULL, NULL, 0) == 0);
    CHECK(model.regs[at] == 0x3410 && model.values[at] == 0x12340000);
    CHECK(model.regs[at + 1] == TCO_RLD && model.values[at + 1] == 1);
    CHECK(model.regs[at + 2] == TCO1_CNT && model.values[at + 2] == 0x100);
    CHECK(itco_open(NULL, NULL, 0) == -EBUSY);
    CHECK(itco_ioctl(NULL, 0x80285700u, (uintptr_t)&info) == 0);
    CHECK(info.options == 0x8180 && info.firmware == 2 && info.identity[0] == 'i');
    value = 3;
    CHECK(itco_ioctl(NULL, 0xC0045706u, (uintptr_t)&value) == 0 && value == 3);
    CHECK((model.words[TCO_TMR / 2] & TCO_MASK) == 5);
    advance(&model, 4);
    CHECK(model.resets == 0 && itco_timeleft(&watchdog) == 0);
    advance(&model, 1);
    CHECK(model.stage == 1 && model.resets == 0);
    CHECK(itco_ioctl(NULL, 0x80045705u, 0) == 0 && model.stage == 0);
    advance(&model, 9);
    CHECK(model.resets == 0);
    advance(&model, 1);
    CHECK(model.resets == 1);
    CHECK(itco_write(NULL, "V", 1) == 1);
    itco_close(NULL);
    CHECK(!watchdog.opened && (model.words[TCO1_CNT / 2] & TCO_HALT));
    advance(&model, 100);
    CHECK(model.resets == 1);
    CHECK(itco_open(NULL, NULL, 0) == 0);
    CHECK(itco_write(NULL, "V", 1) == 1);
    CHECK(itco_write(NULL, "", 0) == 0 && watchdog.magic);
    CHECK(itco_write(NULL, "x", 1) == 1 && !watchdog.magic);
    itco_close(NULL);
    CHECK(!(model.words[TCO1_CNT / 2] & TCO_HALT));
    advance(&model, 10);
    CHECK(model.resets == 2);
    CHECK(itco_open(NULL, NULL, 0) == 0);
    value = 1;
    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == 0);
    CHECK(model.words[TCO1_CNT / 2] & TCO_HALT);
    value = 2;
    CHECK(itco_ioctl(NULL, 0x80045704u, (uintptr_t)&value) == 0);
    CHECK(!(model.words[TCO1_CNT / 2] & TCO_HALT));
    CHECK(itco_timeout(&watchdog, 614) == 0);
    CHECK((model.words[TCO_TMR / 2] & TCO_MASK) == 1023);
    before = watchdog.timeout;
    CHECK(itco_timeout(&watchdog, 615) == -EINVAL && watchdog.timeout == before);
    CHECK(itco_timeout(&watchdog, 2) == -EINVAL);
    CHECK(itco_timeout(&watchdog, -1) == -EINVAL);
    CHECK(itco_ioctl(NULL, 0x80045707u, (uintptr_t)&value) == 0 && value == 614);
    status();
    CHECK(itco_ioctl(NULL, 0x8004570Au, (uintptr_t)&value) == 0);
    CHECK(itco_ioctl(NULL, 0x80045799u, 0) == -ENOTTY);
    CHECK(itco_ioctl(NULL, 0x80045707u, 0) == -EFAULT);
    CHECK(itco_ioctl(NULL, 0xC0045706u, 0) == -EFAULT);
    bad_user = "bad";
    CHECK(itco_write(NULL, bad_user, 3) == -EFAULT);
    halted();
    trial();
    boot_status();
    boot();
    threaded();
    deadline();
    options();
    parser();
    line();
    attach_rows();
    elapsed_rows();
    printf("itco_hosttest: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
