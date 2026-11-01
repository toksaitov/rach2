/*
 * ideapad_hosttest.c: the IdeaPad's VPC2004 driver, rach2/sys/acpi/ideapad.c,
 * against a model of its methods VPCR and VPCW over the controller's command
 * and data registers, with the event device and rfkill as models too.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * `ideapad.c` is included whole, under RACH2_HOSTTEST, which leaves out its
 * ACPICA glue. The model is the Lenovo G50-70's as lenovo.md of milestone 70
 * measured it: _CFG 0xFDE104, the Wi-Fi bit 0, the radio switch 1. Its
 * controller takes a command a number of reads of the command register
 * after it is written, or never; reading the events clears them. The rows
 * check the boot's writes and registrations, the keys of the event bits,
 * bit 9's read of the radio switch, ideapad_wlan's writes, a slow, a stuck
 * and a failing controller, and that every method runs under the lock.
 */
#include "acpi/ideapad.c"

extern int printf(const char *format, ...);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("ideapad_hosttest: line %d: %s\n", (int)__LINE__, #c); \
            fails++;                                                      \
        }                                                                 \
    } while (0)

/* The model of the controller behind VPCR and VPCW. */
static struct {
    uint64_t vcmd, vdat;  /* the command and data registers */
    int data_set;         /* VPCW(0) since the last command */
    unsigned busy;        /* reads of vcmd before a command is taken */
    unsigned left;        /* of those, still to come for this command */
    int stuck;            /* never takes a command */
    int fail;             /* VPCR fails with -EIO */
    uint8_t vpc1, vpc2;   /* the event bits; a read clears them */
    uint64_t wifi, rf;    /* the Wi-Fi bit and the radio switch */
    unsigned wifi_writes; /* W_WIFI commands taken */
    unsigned rf_writes;   /* W_RF commands taken */
    unsigned misuse;      /* a method called without the lock, or a protocol slip */
    uint64_t waited_us;   /* the delays asked for */
    int locked;
    unsigned locks;
} m;

/** Run the command in vcmd as the controller does, and clear it. */
static void model_run(void)
{
    switch (m.vcmd) {
    case VPCCMD_R_VPC1:
        m.vdat = m.vpc1;
        m.vpc1 = 0;
        break;
    case VPCCMD_R_VPC2:
        m.vdat = m.vpc2;
        m.vpc2 = 0;
        break;
    case VPCCMD_R_WIFI:
        m.vdat = m.wifi;
        break;
    case VPCCMD_R_RF:
        m.vdat = m.rf;
        break;
    case VPCCMD_W_WIFI:
        if (!m.data_set)
            m.misuse++;
        m.wifi = m.vdat;
        m.wifi_writes++;
        break;
    case VPCCMD_W_RF:
        if (!m.data_set)
            m.misuse++;
        m.rf = m.vdat;
        m.rf_writes++;
        break;
    default:
        m.misuse++;
        break;
    }
    m.vcmd = 0;
    m.data_set = 0;
}

static int model_vpcr(unsigned reg, uint64_t *value)
{
    if (!m.locked)
        m.misuse++;
    if (m.fail)
        return -EIO;
    if (reg == 1) {
        if (m.vcmd != 0 && !m.stuck) {
            if (m.left == 0)
                model_run();
            else
                m.left--;
        }
        *value = m.vcmd;
        return 0;
    }
    if (m.vcmd != 0)
        m.misuse++; /* the data read before the command was taken */
    *value = m.vdat;
    return 0;
}

static int model_vpcw(unsigned reg, uint64_t value)
{
    if (!m.locked)
        m.misuse++;
    if (reg == 1) {
        if (m.vcmd != 0)
            m.misuse++; /* a command over one not yet taken */
        m.vcmd = value;
        m.left = m.busy;
        return 0;
    }
    m.vdat = value;
    m.data_set = 1;
    return 0;
}

static void model_delay(uint32_t us)
{
    m.waited_us += us;
}

static void model_lock(void)
{
    if (m.locked)
        m.misuse++;
    m.locked = 1;
    m.locks++;
}

static void model_unlock(void)
{
    if (!m.locked)
        m.misuse++;
    m.locked = 0;
}

static const struct ideapad_ops model_ops = {model_vpcr, model_vpcw, model_delay, model_lock,
                                             model_unlock};

/* The event device and the keys it reported. */
struct evdev {
    const char *name, *phys;
    uint16_t bustype;
    uint8_t keys[EVDEV_KEY_BYTES];
};
static struct evdev the_dev;
static uint16_t reported[16];
static int nreported;

struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys)
{
    (void)vendor;
    (void)product;
    the_dev.name = name;
    the_dev.bustype = bustype;
    k_memcpy(the_dev.keys, keys, EVDEV_KEY_BYTES);
    return &the_dev;
}

void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags)
{
    (void)flags;
    dev->phys = phys;
}

void acpi_report_key(struct evdev *dev, uint16_t code)
{
    if (dev != &the_dev)
        m.misuse++;
    if (nreported < 16)
        reported[nreported] = code;
    nreported++;
}

/* rfkill: the one switch registered, and the hard blocks reported. */
struct rfkill {
    const char *name;
    unsigned type;
    int phy;
    const struct rfkill_ops *ops;
    void *data;
    int hard;
    int hw_calls;
};
static struct rfkill the_switch;
static int registered;

int rfkill_register(const char *name, unsigned type, int phy, const struct rfkill_ops *ops,
                    void *data, struct rfkill **out)
{
    registered++;
    the_switch.name = name;
    the_switch.type = type;
    the_switch.phy = phy;
    the_switch.ops = ops;
    the_switch.data = data;
    *out = &the_switch;
    return 0;
}

int rfkill_set_hw_state(struct rfkill *rf, int blocked)
{
    rf->hard = blocked;
    rf->hw_calls++;
    return blocked;
}

static int warnings;

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level <= KLOG_WARN)
        warnings++;
}

/** Start afresh: the G50-70's controller, nothing registered or reported. */
static void fresh(void)
{
    k_memset(&m, 0, sizeof(m));
    k_memset(&pad, 0, sizeof(pad));
    k_memset(&the_dev, 0, sizeof(the_dev));
    k_memset(&the_switch, 0, sizeof(the_switch));
    registered = 0;
    nreported = 0;
    warnings = 0;
    m.wifi = 0;
    m.rf = 1;
    m.busy = 2;
}

/** Whether the device registered `key`. */
static int has_key(uint16_t key)
{
    return (the_dev.keys[key / 8] >> (key % 8)) & 1;
}

/** The G50-70's boot: the device, the radio switch on, ideapad_wlan. */
static void test_attach(void)
{
    fresh();
    m.rf = 0; /* left off, to see the boot turn it on */
    ideapad_attach(&model_ops, 0xFDE104, "\\_SB.PCI0.LPCB.EC0.VPC0");
    CHECK(the_dev.name && strcmp(the_dev.name, "Ideapad extra buttons") == 0);
    CHECK(the_dev.phys && strcmp(the_dev.phys, "ideapad/input0") == 0);
    CHECK(the_dev.bustype == EVDEV_BUS_HOST);
    CHECK(has_key(KEY_WLAN) && has_key(KEY_SWITCHVIDEOMODE) && has_key(KEY_CAMERA));
    CHECK(has_key(KEY_MICMUTE) && has_key(KEY_F16) && !has_key(KEY_POWER));
    CHECK(m.rf_writes == 1 && m.rf == 1);
    CHECK(registered == 1 && strcmp(the_switch.name, "ideapad_wlan") == 0);
    CHECK(the_switch.type == RFKILL_TYPE_WLAN && the_switch.phy == -1);
    CHECK(the_switch.hw_calls == 1 && the_switch.hard == 0);
    CHECK(m.wifi_writes == 0 && m.wifi == 0); /* the boot leaves the Wi-Fi bit */
    CHECK(nreported == 0 && warnings == 0 && m.misuse == 0 && !m.locked);
}

/** A machine whose _CFG has no Wi-Fi gets no switch, and its radio switch
 * is still turned on and read. */
static void test_no_wifi(void)
{
    fresh();
    ideapad_attach(&model_ops, 0xFDE104 & ~((uint64_t)1 << CFG_CAP_WIFI), "VPC0");
    CHECK(registered == 0 && pad.wlan == NULL);
    CHECK(m.rf_writes == 1);
    m.vpc2 = 1u << (VPC_BIT_RF - 8);
    ideapad_event();
    CHECK(nreported == 0 && m.misuse == 0);
}

/** Bit 13, VPC2's bit 5, is KEY_WLAN; the reads clear the events. */
static void test_airplane_key(void)
{
    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.vpc2 = 1u << (13 - 8);
    ideapad_event();
    CHECK(nreported == 1 && reported[0] == KEY_WLAN);
    CHECK(m.vpc1 == 0 && m.vpc2 == 0);
    ideapad_event();
    CHECK(nreported == 1); /* a Notify with no bits gives nothing */
    CHECK(m.misuse == 0 && !m.locked);
}

/** Each key bit gives its key in the table's order; bit 4, the brightness,
 * which the video bus reports, gives none. */
static void test_keys(void)
{
    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.vpc1 = (1u << 4) | (1u << 6) | (1u << 7);
    m.vpc2 = (1u << (8 - 8)) | (1u << (11 - 8)) | (1u << (13 - 8));
    ideapad_event();
    CHECK(nreported == 5);
    CHECK(reported[0] == KEY_SWITCHVIDEOMODE && reported[1] == KEY_CAMERA);
    CHECK(reported[2] == KEY_MICMUTE && reported[3] == KEY_F16 && reported[4] == KEY_WLAN);
    nreported = 0;
    m.vpc1 = 1u << 4;
    ideapad_event();
    CHECK(nreported == 0 && m.misuse == 0);
}

/** Bit 9 reads the radio switch: without a switch of the machine's own, as
 * on the G50-70, the hard block stays clear; with one, a 0 blocks. */
static void test_radio_switch(void)
{
    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.rf = 0;
    m.vpc2 = 1u << (VPC_BIT_RF - 8);
    ideapad_event();
    CHECK(the_switch.hw_calls == 2 && the_switch.hard == 0 && nreported == 0);

    fresh();
    pad.hw_switch = 1;
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    CHECK(m.rf_writes == 0); /* a machine with the switch keeps its own */
    CHECK(the_switch.hard == 0);
    m.rf = 0;
    m.vpc2 = 1u << (VPC_BIT_RF - 8);
    ideapad_event();
    CHECK(the_switch.hard == 1);
    m.rf = 1;
    m.vpc2 = 1u << (VPC_BIT_RF - 8);
    ideapad_event();
    CHECK(the_switch.hard == 0 && m.misuse == 0);
}

/** ideapad_wlan's soft block writes the controller's Wi-Fi bit. */
static void test_soft_block(void)
{
    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.wifi = 1;
    CHECK(the_switch.ops->set_block(the_switch.data, 1) == 0);
    CHECK(m.wifi == 0 && m.wifi_writes == 1);
    CHECK(the_switch.ops->set_block(the_switch.data, 0) == 0);
    CHECK(m.wifi == 1 && m.wifi_writes == 2 && m.misuse == 0 && !m.locked);
}

/** A slow controller is waited for; a stuck one fails after the limit and
 * leaves the lock free; a failing method's error comes back. */
static void test_controller(void)
{
    fresh();
    m.busy = 50;
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.vpc2 = 1u << (13 - 8);
    ideapad_event();
    CHECK(nreported == 1 && reported[0] == KEY_WLAN && warnings == 0);

    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.stuck = 1;
    m.waited_us = 0;
    m.vpc2 = 1u << (13 - 8);
    ideapad_event();
    CHECK(nreported == 0 && warnings == 1 && !m.locked);
    CHECK(m.waited_us >= IDEAPAD_TIMEOUT_US && m.waited_us < 2 * IDEAPAD_TIMEOUT_US);
    m.vcmd = 0;
    CHECK(the_switch.ops->set_block(the_switch.data, 1) == -ETIMEDOUT && !m.locked);

    fresh();
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    m.fail = 1;
    m.vpc2 = 1u << (13 - 8);
    ideapad_event();
    CHECK(nreported == 0 && !m.locked);
    CHECK(the_switch.ops->set_block(the_switch.data, 1) == -EIO && !m.locked);

    fresh();
    m.stuck = 1;
    ideapad_attach(&model_ops, 0xFDE104, "VPC0");
    CHECK(registered == 1 && the_switch.hw_calls == 0 && !m.locked);
}

int main(void)
{
    test_attach();
    test_no_wifi();
    test_airplane_key();
    test_keys();
    test_radio_switch();
    test_soft_block();
    test_controller();
    printf("ideapad_hosttest: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
