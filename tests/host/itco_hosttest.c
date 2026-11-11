/* Intel TCO register sequencing, two-stage expiry, watchdog file ABI and
 * the kernel's feed before a program opens the device.
 * host-deps:
 */
extern int printf(const char *format, ...);

#undef RACH2_HOSTTEST
#include "arch/x86/itco.c"

struct model {
    uint16_t words[16];
    uint32_t gcs;
    unsigned left, stage, resets, writes;
    unsigned regs[128];
    uint32_t values[128];
    int locked, absent, stuck_timer, stuck_halt;
};

static int failures;
static struct model model;
static const void *bad_user;

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
/** No command line here: the rows set the options' fields. */
int kcmdline_count(const char *key)
{
    (void)key;
    return 0;
}
/** No command line here. */
int kcmdline_value_nth(const char *key, int nth, char *out, unsigned int outlen)
{
    (void)key;
    (void)nth;
    (void)out;
    (void)outlen;
    return 0;
}
/** No command line here. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}
/** No command line here. */
int kcmdline_param_bool(const char *key, int dflt)
{
    (void)key;
    return dflt;
}
/** No command line here, so nothing is parsed. */
int k_strtoll(const char *text, size_t len, long long *out)
{
    (void)text;
    (void)len;
    *out = 0;
    return -1;
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
/** Diagnostics have no side effect in the model. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
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

/** Run `seconds` of the kernel's tick at 5 a second, the model's 0.6 s
 * tick every third, and watchdogd after each tick when `threads` is
 * nonzero. */
static void run_threads(unsigned seconds, int threads)
{
    unsigned i;

    for (i = 1; i <= seconds * 5u; i++) {
        itco_feed_tick(&watchdog);
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
    itco_feed_tick(&watchdog);
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
    itco_feed_tick(&watchdog);
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

/** Check actual writes, expiry, failures, exclusive open and the ioctl ABI. */
int main(void)
{
    int value, before;
    unsigned at;
    struct {
        uint32_t options, firmware;
        char identity[32];
    } info;

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
    boot();
    threaded();
    deadline();
    options();
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
    CHECK(itco_attach(&watchdog, 30) == -EIO);
    printf("itco_hosttest: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
