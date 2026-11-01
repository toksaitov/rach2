/*
 * ideapad.c: the Lenovo IdeaPad's ACPI device VPC2004, as Linux's
 * ideapad-laptop drives it: its keys, the airplane key among them, as the
 * event device "Ideapad extra buttons", and its Wi-Fi switch in rfkill.
 */

#include "../evdev.h"
#include "../klog.h"
#include "../rfkill.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

#ifndef RACH2_HOSTTEST
#include "acpi.h"
#endif

#include "devices.h"

/*
 * The controller's commands, Linux's VPCCMD_ values. A command goes to the
 * controller's command register through VPCW(1, cmd), its data through
 * VPCW(0, data); VPCR(1) reads the command register, which the controller
 * clears when it is done, and VPCR(0) the data register.
 */
#define VPCCMD_R_VPC1 0x10 /* the low byte of the events */
#define VPCCMD_R_WIFI 0x14 /* the controller's Wi-Fi bit */
#define VPCCMD_W_WIFI 0x15
#define VPCCMD_R_VPC2 0x1A /* the high byte of the events */
#define VPCCMD_R_RF   0x23 /* the radio switch: 1 lets the radios run */
#define VPCCMD_W_RF   0x24

/* The bit of _CFG that says the machine has Wi-Fi (Linux's CFG_CAP_WIFI_BIT). */
#define CFG_CAP_WIFI 18

/* The event bit after which the radio switch is read again. */
#define VPC_BIT_RF 9

/* A command's wait for the controller: at least 200 ms in all, Linux's
 * IDEAPAD_EC_TIMEOUT, of pauses between reads of the command register. */
#define IDEAPAD_TIMEOUT_US 200000u
#define IDEAPAD_POLL_US    100u

/* The event bits that are keys, and the keys, as Linux's ideapad_keymap has
 * them. The G50-70's airplane key is taken to be bit 13 until it is pressed
 * on the machine; the others are reported where a model has them. */
static const struct {
    unsigned bit;
    uint16_t key;
} vpc_keys[] = {
    {6, KEY_SWITCHVIDEOMODE}, {7, KEY_CAMERA}, {8, KEY_MICMUTE}, {11, KEY_F16}, {13, KEY_WLAN}};

/*
 * How the driver reaches the device: ACPICA's evaluation of VPCR and VPCW,
 * a pause, and the lock that keeps one command at a time, or a host test's
 * model of them. `vpcr` and `vpcw` return 0 or a negative errno.
 */
struct ideapad_ops {
    int (*vpcr)(unsigned reg, uint64_t *value);
    int (*vpcw)(unsigned reg, uint64_t value);
    void (*delay)(uint32_t us);
    void (*lock)(void);
    void (*unlock)(void);
};

/* The one VPC2004 device driven. */
static struct {
    const struct ideapad_ops *ops;
    struct evdev *dev;
    struct rfkill *wlan; /* ideapad_wlan, when _CFG has Wi-Fi */
    uint64_t cfg;        /* what _CFG returned */
    /* Whether VPCCMD_R_RF is a hard block, as on the machines with a radio
     * switch that Linux lists by name; the G50-70 has none, and Rach2 lists
     * none, so the radio switch is only logged. */
    int hw_switch;
} pad;

/** Wait until the controller clears its command register: 0, the error of
 * VPCR, or -ETIMEDOUT. Called with the lock held. */
static int vpc_wait(void)
{
    uint64_t cmd;
    uint32_t waited;
    int rc;

    for (waited = 0;; waited += IDEAPAD_POLL_US) {
        rc = pad.ops->vpcr(1, &cmd);
        if (rc != 0 || cmd == 0) {
            return rc;
        }
        if (waited >= IDEAPAD_TIMEOUT_US) {
            return -ETIMEDOUT;
        }
        pad.ops->delay(IDEAPAD_POLL_US);
    }
}

/**
 * Read the controller's answer to `cmd` into `*value`, as Linux's
 * read_ec_data: the command, a wait until the controller has taken it, and
 * the data register. 0, or the error of a step, which leaves `*value`
 * alone. Process context; it sleeps.
 */
static int vpc_read(unsigned cmd, uint64_t *value)
{
    int rc;

    pad.ops->lock();
    rc = pad.ops->vpcw(1, cmd);
    if (rc == 0) {
        rc = vpc_wait();
    }
    if (rc == 0) {
        rc = pad.ops->vpcr(0, value);
    }
    pad.ops->unlock();
    if (rc != 0) {
        klog(KLOG_WARN, "acpi", "ideapad: read 0x%x: error %d", cmd, rc);
    }
    return rc;
}

/** Give the controller `cmd` with `data`, as Linux's write_ec_cmd: the
 * data, the command, and a wait until the controller has taken it. 0, or
 * the error of a step. Process context; it sleeps. */
static int vpc_write(unsigned cmd, uint64_t data)
{
    int rc;

    pad.ops->lock();
    rc = pad.ops->vpcw(0, data);
    if (rc == 0) {
        rc = pad.ops->vpcw(1, cmd);
    }
    if (rc == 0) {
        rc = vpc_wait();
    }
    pad.ops->unlock();
    if (rc != 0) {
        klog(KLOG_WARN, "acpi", "ideapad: write 0x%x: error %d", cmd, rc);
    }
    return rc;
}

/** Read the radio switch and give ideapad_wlan its hard block from it on a
 * machine that has the switch, as Linux's ideapad_sync_rfk_state; on one
 * without, the hard block stays clear and the value is only logged. */
static void ideapad_sync_rf(void)
{
    uint64_t rf;

    if (vpc_read(VPCCMD_R_RF, &rf) != 0) {
        return;
    }
    klog(KLOG_DEBUG, "acpi", "ideapad: radio switch %u", (unsigned)rf);
    if (pad.wlan != NULL) {
        (void)rfkill_set_hw_state(pad.wlan, pad.hw_switch && rf == 0);
    }
}

/**
 * A Notify on the device, whatever its value, as Linux takes it: the events
 * are the bits of VPCCMD_R_VPC1 and, above them, of VPCCMD_R_VPC2, which the
 * reads clear. A key's bit is reported as its key, bit 9 reads the radio
 * switch again, and the others, the brightness among them, which the video
 * bus reports, are only logged. On kacpid.
 */
static void ideapad_event(void)
{
    uint64_t vpc1, vpc2;
    unsigned bits, i;

    if (vpc_read(VPCCMD_R_VPC1, &vpc1) != 0 || vpc_read(VPCCMD_R_VPC2, &vpc2) != 0) {
        return;
    }
    bits = (unsigned)(((vpc2 & 0xff) << 8) | (vpc1 & 0xff));
    klog(KLOG_DEBUG, "acpi", "event: ideapad: bits 0x%04x", bits);
    for (i = 0; i < sizeof(vpc_keys) / sizeof(vpc_keys[0]); i++) {
        if (bits & (1u << vpc_keys[i].bit)) {
            klog(KLOG_DEBUG, "acpi", "event: ideapad: bit %u: key %u", vpc_keys[i].bit,
                 (unsigned)vpc_keys[i].key);
            acpi_report_key(pad.dev, vpc_keys[i].key);
        }
    }
    if (bits & (1u << VPC_BIT_RF)) {
        ideapad_sync_rf();
    }
}

/** ideapad_wlan's soft block, written to the controller's Wi-Fi bit as
 * Linux writes it: 0 blocks, 1 lets the radio run. */
static int ideapad_set_block(void *data, int blocked)
{
    (void)data;
    return vpc_write(VPCCMD_W_WIFI, blocked ? 0 : 1);
}

static const struct rfkill_ops ideapad_rfkill_ops = {ideapad_set_block};

/**
 * Take the device at `path`, whose _CFG gave `cfg`, through `ops`, as Linux's
 * ideapad_acpi_add: the event device of its keys; the radio switch turned
 * on, VPCCMD_W_RF 1, on a machine without a switch of its own, so that the
 * controller blocks no radio of its own accord; ideapad_wlan when the
 * machine has Wi-Fi; and the radio switch read. ideapad_wlan starts as
 * rfkill starts it and its soft block is written to the controller only
 * when it changes, so the boot leaves the controller's Wi-Fi bit as the
 * firmware set it; VPCCMD_R_WIFI is read for the record. Process context.
 */
static void ideapad_attach(const struct ideapad_ops *ops, uint64_t cfg, const char *path)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    uint64_t wifi = 0;
    unsigned i;
    int rc;

    pad.ops = ops;
    pad.cfg = cfg;
    k_memset(keys, 0, sizeof(keys));
    for (i = 0; i < sizeof(vpc_keys) / sizeof(vpc_keys[0]); i++) {
        keys[vpc_keys[i].key / 8] |= (uint8_t)(1u << (vpc_keys[i].key % 8));
    }
    pad.dev = evdev_register("Ideapad extra buttons", EVDEV_BUS_HOST, 0, 0, keys);
    evdev_describe(pad.dev, "ideapad/input0", 0);
    if (!pad.hw_switch) {
        (void)vpc_write(VPCCMD_W_RF, 1);
    }
    if (cfg & ((uint64_t)1 << CFG_CAP_WIFI)) {
        (void)vpc_read(VPCCMD_R_WIFI, &wifi);
        rc = rfkill_register("ideapad_wlan", RFKILL_TYPE_WLAN, -1, &ideapad_rfkill_ops, &pad,
                             &pad.wlan);
        if (rc != 0) {
            klog(KLOG_WARN, "acpi", "ideapad: rfkill: error %d", rc);
        }
    }
    klog(KLOG_DEBUG, "acpi", "ideapad: %s: cfg 0x%x, wifi %u%s", path, (unsigned)cfg,
         (unsigned)wifi, pad.wlan != NULL ? ", ideapad_wlan" : "");
    ideapad_sync_rf();
}

#ifndef RACH2_HOSTTEST

static ACPI_HANDLE vpc_handle;
static ACPI_SEMAPHORE vpc_lock;

/** The errno of an ACPICA status: AE_TIME, the controller's timeout, is
 * -ETIMEDOUT, and any other failure -EIO. */
static int vpc_errno(ACPI_STATUS status)
{
    if (ACPI_SUCCESS(status)) {
        return 0;
    }
    return status == AE_TIME ? -ETIMEDOUT : -EIO;
}

/** VPCR(reg), an integer. */
static int acpi_vpcr(unsigned reg, uint64_t *value)
{
    static char vpcr[] = "VPCR";
    ACPI_OBJECT arg, result;
    ACPI_OBJECT_LIST args;
    ACPI_BUFFER ret;
    ACPI_STATUS status;

    arg.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = reg;
    args.Count = 1;
    args.Pointer = &arg;
    ret.Length = sizeof(result);
    ret.Pointer = &result;
    status = AcpiEvaluateObjectTyped(vpc_handle, vpcr, &args, &ret, ACPI_TYPE_INTEGER);
    if (ACPI_SUCCESS(status)) {
        *value = result.Integer.Value;
    }
    return vpc_errno(status);
}

/** VPCW(reg, value). */
static int acpi_vpcw(unsigned reg, uint64_t value)
{
    static char vpcw[] = "VPCW";
    ACPI_OBJECT arg[2];
    ACPI_OBJECT_LIST args;

    arg[0].Type = ACPI_TYPE_INTEGER;
    arg[0].Integer.Value = reg;
    arg[1].Type = ACPI_TYPE_INTEGER;
    arg[1].Integer.Value = value;
    args.Count = 2;
    args.Pointer = arg;
    return vpc_errno(AcpiEvaluateObject(vpc_handle, vpcw, &args, NULL));
}

static void acpi_vpc_delay(uint32_t us)
{
    AcpiOsStall(us);
}

/* A semaphore of one unit, on which a process sleeps and the boot spins,
 * as the embedded controller's: a command waits for the controller, and
 * a command of rfkill's must not run inside one of the Notify's. */
static void acpi_vpc_lock(void)
{
    (void)AcpiOsWaitSemaphore(vpc_lock, 1, ACPI_WAIT_FOREVER);
}

static void acpi_vpc_unlock(void)
{
    (void)AcpiOsSignalSemaphore(vpc_lock, 1);
}

static const struct ideapad_ops acpi_ops = {acpi_vpcr, acpi_vpcw, acpi_vpc_delay, acpi_vpc_lock,
                                            acpi_vpc_unlock};

/** A Notify on the device, on kacpid. */
static void vpc_notify(ACPI_HANDLE device, UINT32 value, void *context)
{
    (void)device;
    (void)context;
    klog(KLOG_DEBUG, "acpi", "event: ideapad: notify 0x%x", (unsigned)value);
    ideapad_event();
}

/** The first present VPC2004 device; the walk ends at it. */
static ACPI_STATUS vpc_found(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    (void)depth;
    (void)context;
    (void)ret;
    vpc_handle = handle;
    return AE_CTRL_TERMINATE;
}

/**
 * Take the first present VPC2004 device whose _CFG answers, as Linux's
 * ideapad-laptop does, and its Notify. _CFG is evaluated as Linux
 * evaluates it; on the G50-70 it sets the firmware's APFG, and under
 * Windows 7's OSYS it would also set the controller's Wi-Fi and Bluetooth
 * bits.
 */
void acpi_ideapad_start(void)
{
    static char hid[] = "VPC2004", cfg_name[] = "_CFG";
    char path[64];
    ACPI_OBJECT cfg;
    ACPI_BUFFER buf;
    ACPI_STATUS status;

    (void)AcpiGetDevices(hid, vpc_found, NULL, NULL);
    if (vpc_handle == NULL) {
        return;
    }
    buf.Length = sizeof(path);
    buf.Pointer = path;
    if (ACPI_FAILURE(AcpiGetName(vpc_handle, ACPI_FULL_PATHNAME_NO_TRAILING, &buf))) {
        path[0] = '\0';
    }
    buf.Length = sizeof(cfg);
    buf.Pointer = &cfg;
    status = AcpiEvaluateObjectTyped(vpc_handle, cfg_name, NULL, &buf, ACPI_TYPE_INTEGER);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "ideapad: refused: %s: _CFG: %s", path,
             AcpiFormatException(status));
        return;
    }
    if (ACPI_FAILURE(AcpiOsCreateSemaphore(1, 1, &vpc_lock))) {
        klog(KLOG_ERR, "acpi", "ideapad: refused: %s: no lock", path);
        return;
    }
    ideapad_attach(&acpi_ops, cfg.Integer.Value, path);
    status = AcpiInstallNotifyHandler(vpc_handle, ACPI_DEVICE_NOTIFY, vpc_notify, NULL);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "ideapad: notify: %s", AcpiFormatException(status));
    }
}

#endif /* RACH2_HOSTTEST */
