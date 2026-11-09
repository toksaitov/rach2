/* Intel TCO register sequencing, two-stage expiry and watchdog file ABI.
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
/** The node is tested through its production operations directly. */
int32_t devfs_node_register(struct devfs_node *node)
{
    (void)node;
    return 0;
}
/** Diagnostics have no side effect in the model. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
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
    CHECK(itco_attach(&watchdog) == 0);
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
    CHECK(itco_ioctl(NULL, 0x80045701u, (uintptr_t)&value) == 0 && value == 0);
    CHECK(itco_ioctl(NULL, 0x80045702u, (uintptr_t)&value) == 0 && value == 0);
    CHECK(itco_ioctl(NULL, 0x8004570Au, (uintptr_t)&value) == 0);
    CHECK(itco_ioctl(NULL, 0x80045799u, 0) == -ENOTTY);
    CHECK(itco_ioctl(NULL, 0x80045707u, 0) == -EFAULT);
    CHECK(itco_ioctl(NULL, 0xC0045706u, 0) == -EFAULT);
    bad_user = "bad";
    CHECK(itco_write(NULL, bad_user, 3) == -EFAULT);
    setup();
    model.absent = 1;
    CHECK(itco_attach(&watchdog) == -EIO && model.writes == 0);
    setup();
    model.locked = 1;
    CHECK(itco_attach(&watchdog) == 0);
    CHECK(itco_open(NULL, NULL, 0) == -EIO && !watchdog.opened);
    CHECK(model.words[TCO1_CNT / 2] & TCO_HALT);
    setup();
    model.stuck_timer = 1;
    CHECK(itco_attach(&watchdog) == -EIO);
    setup();
    model.stuck_halt = 1;
    model.words[TCO1_CNT / 2] &= (uint16_t)~TCO_HALT;
    CHECK(itco_attach(&watchdog) == -EIO);
    printf("itco_hosttest: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
