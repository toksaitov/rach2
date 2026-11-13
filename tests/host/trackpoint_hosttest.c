/**
 * The device behind a Synaptics touchpad's pass-through port, the quirks
 * Linux 6.12 chooses by the PS/2 mouse's PNP ID, and the reconnect of a
 * pointer that lost its packets' form or reset itself: a TrackPoint of each
 * kind Linux's trackpoint.c knows, IBM, ALPS, Elan, NXP, JYT Synaptics and
 * Synaptics, or a mouse, found, set up, reported and adjusted through its
 * files in /sys (sys/arch/x86/trackpoint.c, ps2mouse.c); the PNP IDs and
 * their firmware ID (i8042.c); the pads of Linux's `min_max_pnpid_table`,
 * `topbuttonpad_pnp_ids`, `forcepad_pnp_ids` and `smbus_pnp_ids`
 * (synaptics.c); and the reconnect after five bad packets in a row or the
 * announcement 0xAA 0x00. Against a model of the 8042, a Synaptics touchpad
 * and the guest behind it, with the event devices read as a program reads
 * them (evdev.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/park_host.c
 *
 * The driver's files are included whole, so that their static state is
 * reachable. The model touchpad answers as the Lenovo's of
 * synaptics_hosttest.c, firmware 8.1, with what a row turns on beside: the
 * pass-through port, three extra buttons wired to the stick, a clickpad,
 * SMBus, a board ID, another model ID, knocks left unanswered, or a plain
 * mouse in its place. It takes a request sliced: four resolutions make a
 * byte, which a status request answers as a query and the rates 0x14 and
 * 0xC8 take as the mode and the gesture mode, and which the rate 0x28 sends
 * to the guest. Each byte the guest sends back comes as byte 1 of a
 * pass-through packet, 0x84 b 0 0xC4 0 0.
 *
 * The guest is none, a mouse that the knocks give a wheel, ID 3, or an
 * Explorer's, ID 4, or a TrackPoint of a kind, firmware and button count. A
 * TrackPoint's RAM has 256 locations, which its 0xE2 operations read, write
 * and toggle, and which its power-on reset, 0xE2 0x7F, sets to the values
 * IBM gives, answering 0xAA 0x00, or for a row 0xAA 0x01 with the RAM
 * kept. A mouse refuses the read-ID command 0xE1 with 0xFE.
 *
 * The rows compare the bytes the touchpad and the guest took with those
 * Linux 6.12 sends, as its psmouse-base.c, synaptics.c, trackpoint.c and
 * libps2.c have them: `psmouse_probe`, `psmouse_extensions` on a
 * pass-through port, `trackpoint_detect`, `trackpoint_sync`,
 * `trackpoint_reconnect`, `synaptics_pt_write`, `synaptics_reconnect`,
 * `__psmouse_reconnect` and `psmouse_attr_set_helper`; and the event
 * devices' names, numbers, properties and events with what Linux's give.
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2mouse.c"
#include "arch/x86/synaptics.c"
#include "arch/x86/trackpoint.c"

#include "fs/fd.h"
#include "lib/kerrno.h"
#include "proc/wait_queue.h"

int printf(const char *format, ...);
int sprintf(char *s, const char *format, ...);
int strcmp(const char *a, const char *b);

int park_run(void (*fn)(void));
int park_resume(void);
void park(void);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("trackpoint-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
    } while (0)

/* The status of a byte the device sent, and the configuration byte the
 * firmware leaves. */
#define MOUSE_BYTE      (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)
#define FIRMWARE_CONFIG 0x65

/* The ticks of the kernel's clock, which `ps2mouse_msleep` and the rows
 * move on. */
volatile uint64_t kernel_ticks;

/* The model 8042's output buffer, a queue of bytes the device sent. */
#define MODEL_BYTES 1024
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;

/* The controller's configuration byte, and what the next write to port
 * 0x60 is for: 0, or the command that named it. */
static uint8_t ctl_config;
static uint8_t ctl_next;

/* The touchpad: a plain mouse in its place, and whether that mouse has a
 * wheel, answering every read-ID with the IntelliMouse's 3; its
 * capabilities, extended capabilities and those of queries 0x0c and 0x10,
 * its board and the last byte of its model ID; how many knocks it leaves
 * unanswered from now; the command whose argument comes next; the byte the
 * resolutions since the last other command make, and their count; its mode
 * byte, gesture mode and reporting; and every byte it took. */
static int dev_plain;
static int dev_tp; /* the auxiliary device is the TrackPoint itself */
static int dev_wheel;
static uint32_t pad_caps;
static uint32_t pad_ext;
static uint32_t pad_ext_0c;
static uint32_t pad_ext_10;
static unsigned int pad_board;
static uint8_t pad_model_low;
static int knock_misses;
static uint8_t arg_for;
static uint8_t special;
static int res_count;
static uint8_t pad_mode;
static int agm_on;
static int pad_reporting;
#define TOOK_MAX 2048
static uint8_t took[TOOK_MAX];
static int n_took;

/* The bits a row turns on: the pass-through port, three extra buttons, the
 * stick's buttons wired to the pad, a clickpad and SMBus. */
#define PAD_PASS_THROUGH 0x000080u
#define PAD_THREE_EXTRA  0x003000u
#define PAD_STICK_10     0x010000u
#define PAD_CLICKPAD     0x100000u
#define PAD_INTERTOUCH   0x004000u

/* The guest's kinds. */
#define GUEST_NONE  0
#define GUEST_MOUSE 1
#define GUEST_TP    2

/* The guest: its kind; the ID the knocks give a mouse, 0 for none; the ID
 * it answers now and the last three rates; a TrackPoint's kind, firmware
 * and buttons, whether its power-on reset fails, whether it refuses the
 * extended button query, its RAM and where it is in an 0xE2 operation;
 * the command whose argument comes next; whether it reports; and every
 * byte it took. */
static int guest_kind;
static uint8_t guest_wheel;
static uint8_t guest_id;
static uint8_t guest_rates[3];
static uint8_t tp_kind;
static uint8_t tp_fw;
static uint8_t tp_buttons;
static int tp_por_fails;
static int tp_refuse_ext;
static uint8_t tp_ram[256];
static int tp_stage;
static uint8_t tp_op;
static uint8_t tp_loc;
static uint8_t g_arg_for;
static int guest_reporting;
static uint8_t gtook[TOOK_MAX];
static int n_gtook;

/* The handler registered, and the warnings logged. */
static isr_handler_t registered_handler;
static int warnings;
static const char *last_warning;
static const char *last_info;

/* The files a driver added to a serio port: the function a protocol gave,
 * the driver's own and the times the driver's were bound, and each file
 * `sysfs_add_attr` was asked for. */
static int (*files_add)(int dir);
static int files_port;
static int (*driver_add)(int dir);
static int driver_binds;
#define ATTRS_MAX 32
static struct {
    int parent;
    char name[24];
    uint32_t mode;
    sysfs_show_t show;
    sysfs_store_t store;
    int arg;
} attr_made[ATTRS_MAX];
static int n_attrs;

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("trackpoint-test: fail: the model's output buffer overflowed\n");
        fails++;
        return;
    }
    model_data[model_head + model_count] = data;
    model_count++;
}

/** Put the three bytes `a`, `b`, `c` in the output buffer. */
static void put3(uint8_t a, uint8_t b, uint8_t c)
{
    model_put(a);
    model_put(b);
    model_put(c);
}

/** Put the 24 bits of `v` in the output buffer, high byte first. */
static void put24(uint32_t v)
{
    put3((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

/** Put the guest's byte `b` in the output buffer, in a pass-through
 * packet. */
static void pt_put(uint8_t b)
{
    if (dev_tp) {
        model_put(b);
        return;
    }
    model_put(0x84);
    model_put(b);
    model_put(0x00);
    model_put(0xC4);
    model_put(0x00);
    model_put(0x00);
}

/** Set the TrackPoint's RAM to its power-on values, as IBM's TrackPoint
 * System Engineering document gives them and Linux's TP_DEF_* repeat. */
static void tp_power_on(void)
{
    k_memset(tp_ram, 0, sizeof(tp_ram));
    tp_ram[0x4A] = 0x80; /* sensitivity */
    tp_ram[0x4D] = 0x06; /* inertia */
    tp_ram[0x60] = 0x61; /* speed */
    tp_ram[0x57] = 0x0A; /* reach */
    tp_ram[0x58] = 0xFF; /* draghys */
    tp_ram[0x59] = 0x14; /* mindrag */
    tp_ram[0x5C] = 0x08; /* thresh */
    tp_ram[0x5A] = 0xFF; /* upthresh */
    tp_ram[0x5E] = 0x26; /* ztime */
    tp_ram[0x5D] = 0x87; /* jenks */
    tp_ram[0x5F] = 0x05; /* drift_time */
    tp_ram[0x4B] = tp_buttons;
}

/** Take `b`, a byte of a TrackPoint's 0xE2 operation at stage
 * `tp_stage`: the operation, then its location and value. */
static void tp_take(uint8_t b)
{
    if (tp_stage == 1 && b == 0x4B && tp_refuse_ext) {
        pt_put(0xFE);
        tp_stage = 0;
        return;
    }
    pt_put(0xFA);
    if (tp_stage == 2) {
        tp_loc = b;
        tp_stage = 3;
        return;
    }
    if (tp_stage == 3) {
        tp_stage = 0;
        if (tp_op == 0x81) {
            tp_ram[tp_loc] = b;
        } else {
            tp_ram[tp_loc] ^= b;
        }
        return;
    }
    tp_op = b;
    tp_stage = 0;
    if (b == 0x7F) {
        if (!tp_por_fails) {
            tp_power_on();
        }
        pt_put(0xAA);
        pt_put(tp_por_fails ? 0x01 : 0x00);
    } else if (b == 0x81 || b == 0x47) {
        tp_stage = 2;
    } else {
        pt_put(tp_ram[b]);
    }
}

/** Take `b`, the argument of the command `cmd` before it: a mouse's ID
 * changes after the knocks it knows. */
static void guest_arg(uint8_t cmd, uint8_t b)
{
    pt_put(0xFA);
    if (cmd != 0xF3) {
        return;
    }
    guest_rates[0] = guest_rates[1];
    guest_rates[1] = guest_rates[2];
    guest_rates[2] = b;
    if (guest_rates[0] == 200 && guest_rates[1] == 100 && guest_rates[2] == 80 &&
        guest_wheel >= 3) {
        guest_id = 3;
    } else if (guest_rates[0] == 200 && guest_rates[1] == 200 && guest_rates[2] == 80 &&
               guest_wheel == 4) {
        guest_id = 4;
    }
}

/** Take `b`, a command of the PS/2 mouse's or a TrackPoint's own. */
static void guest_command(uint8_t b)
{
    if (b == 0xE1 || b == 0xE2) {
        pt_put(guest_kind == GUEST_TP ? 0xFA : 0xFE);
        if (guest_kind == GUEST_TP && b == 0xE1) {
            pt_put(tp_kind);
            pt_put(tp_fw);
        } else if (guest_kind == GUEST_TP) {
            tp_stage = 1;
        }
        return;
    }
    pt_put(0xFA);
    switch (b) {
    case 0xE8:
    case 0xF3:
        g_arg_for = b;
        break;
    case 0xF2:
        pt_put(guest_id);
        break;
    case 0xE9: /* the status, which no Synaptics identify reads as a pad's */
        pt_put(guest_reporting ? 0x20 : 0x00);
        pt_put(0x02);
        pt_put(0x64);
        break;
    case 0xFF:
        pt_put(0xAA);
        pt_put(0x00);
        guest_id = 0;
        guest_reporting = 0;
        k_memset(guest_rates, 0, sizeof(guest_rates));
        if (guest_kind == GUEST_TP) {
            tp_power_on();
        }
        break;
    case 0xF4:
        guest_reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        guest_reporting = 0;
        break;
    default:
        break;
    }
}

/** Take `b`, a byte the touchpad sent on to the guest. */
static void guest_take(uint8_t b)
{
    uint8_t cmd;

    if (n_gtook < TOOK_MAX) {
        gtook[n_gtook] = b;
    }
    n_gtook++;
    if (guest_kind == GUEST_NONE) {
        return;
    }
    if (g_arg_for) {
        cmd = g_arg_for;
        g_arg_for = 0;
        guest_arg(cmd, b);
    } else if (tp_stage) {
        tp_take(b);
    } else {
        guest_command(b);
    }
}

/** Answer the query `q` as the touchpad answers it. */
static void answer_query(uint8_t q)
{
    switch (q) {
    case 0x00: /* identify: minor 1, 0x47, model 1 and major 8 */
        if (knock_misses > 0) {
            knock_misses--;
            put3(0, 0, 0);
        } else {
            put3(0x01, 0x47, 0x18);
        }
        break;
    case 0x01: /* modes: the board, query 0x10's bit, then the mode */
        put3((uint8_t)(((pad_board >> 6) & 0xfcu) | (pad_ext_10 ? 0x02u : 0u)),
             (uint8_t)(pad_board & 0xffu), pad_mode);
        break;
    case 0x02:
        put24(pad_caps);
        break;
    case 0x03:
        put3(0x01, 0xe2, pad_model_low);
        break;
    case 0x08: /* 42 and 59 units a millimetre */
        put3(42, 0x80, 59);
        break;
    case 0x09:
        put24(pad_ext);
        break;
    case 0x0a:
        put3(0x18, 0x23, 0x33);
        break;
    case 0x0c:
        put24(pad_ext_0c);
        break;
    case 0x0d: /* 5670 and 4794 */
        put3(0xb1, 0xd3, 0x95);
        break;
    case 0x0f: /* 1314 and 1098 */
        put3(0x29, 0x51, 0x22);
        break;
    case 0x10:
        put24(pad_ext_10);
        break;
    default:
        put3(0, 0, 0);
        break;
    }
}

/** Take `b`, a byte the controller sent on to the touchpad. */
static void dev_take(uint8_t b)
{
    uint8_t cmd;

    if (n_took < TOOK_MAX) {
        took[n_took] = b;
    }
    n_took++;
    if ((ctl_config & 0x20) != 0) {
        return; /* its clock is off */
    }
    if (dev_tp) {
        guest_take(b);
        return;
    }
    if (arg_for) {
        cmd = arg_for;
        arg_for = 0;
        model_put(0xFA);
        if (cmd == 0xE8) {
            special = (uint8_t)(special << 2 | (b & 3u));
            res_count++;
            return;
        }
        if (!dev_plain && res_count >= 4 && b == 0x14) {
            pad_mode = special;
        } else if (!dev_plain && res_count >= 4 && b == 0xC8) {
            agm_on = 1;
        } else if (!dev_plain && res_count >= 4 && b == 0x28 && (pad_caps & PAD_PASS_THROUGH)) {
            guest_take(special);
        }
        res_count = 0;
        return;
    }
    model_put(0xFA);
    switch (b) {
    case 0xE8:
    case 0xF3:
        arg_for = b;
        return;
    case 0xE9:
        if (!dev_plain && res_count >= 4) {
            answer_query(special);
        } else {
            put3(pad_reporting ? 0x20 : 0x00, 2, 100);
        }
        break;
    case 0xF2:
        model_put(dev_plain && dev_wheel ? 0x03 : 0x00);
        break;
    case 0xFF:
        model_put(0xAA);
        model_put(0x00);
        pad_mode = 0;
        agm_on = 0;
        pad_reporting = 0;
        break;
    case 0xF4:
        pad_reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        pad_reporting = 0;
        break;
    default:
        break;
    }
    res_count = 0;
}

uint8_t inb(uint16_t port)
{
    uint8_t data;

    if (port == PS2_STATUS_PORT) {
        return model_count > 0 ? MOUSE_BYTE : 0;
    }
    if (port == PS2_DATA_PORT && model_count > 0) {
        data = model_data[model_head];
        model_head++;
        model_count--;
        return data;
    }
    return 0;
}

void outb(uint16_t port, uint8_t val)
{
    uint8_t next = ctl_next;

    if (port == PS2_CMD_PORT) {
        ctl_next = 0;
        if (val == PS2_CMD_ENABLE_MOUSE) {
            ctl_config &= ~0x20;
        } else if (val == PS2_CMD_READ_CONFIG) {
            model_put(ctl_config);
        } else if (val == PS2_CMD_WRITE_CONFIG || val == PS2_CMD_WRITE_MOUSE) {
            ctl_next = val;
        }
    } else if (port == PS2_DATA_PORT) {
        ctl_next = 0;
        if (next == PS2_CMD_WRITE_CONFIG) {
            ctl_config = val;
        } else if (next == PS2_CMD_WRITE_MOUSE) {
            dev_take(val);
        }
    }
}

void arch_irq_disable(void)
{}

void arch_irq_enable(void)
{}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)name;
    registered_handler = handler;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
        last_warning = format;
    } else if (level == KLOG_INFO) {
        last_info = format;
    }
}

/** No ALPS pad answers where the Synaptics pad does not; the ALPS pad's
 * own rows are alps_hosttest.c's. */
int alps_init(void)
{
    return 0;
}

/** No ALPS pad's set-up fails, so no mouse takes its name. */
const char *alps_mouse_name(const char *proto)
{
    return proto;
}

/** No ALPS pad's bytes come, since `alps_init` found none. */
int alps_byte(uint8_t data)
{
    (void)data;
    return ALPS_GOOD;
}

/* The rest of alps.h, which a port without an ALPS pad never calls;
 * alps_hosttest.c tests it. */
void alps_packet_restart(void)
{}

int alps_reset_after(void)
{
    return 12;
}

int alps_packet_start(void)
{
    return 1;
}

int alps_reconnect(void)
{
    return -1;
}

void alps_disconnect(void)
{}

/** No Elantech pad answers where the Synaptics pad does not; the
 * Elantech pad's own rows are elantech_hosttest.c's. */
int elantech_init(void)
{
    return 0;
}

/** No Elantech pad's bytes come, since `elantech_init` found none. */
int elantech_byte(uint8_t data)
{
    (void)data;
    return ELANTECH_GOOD;
}

/* The rest of elantech.h, which these rows never call. */
void elantech_packet_restart(void)
{}

int elantech_packet_start(void)
{
    return 1;
}

int elantech_reconnect(void)
{
    return -1;
}

void elantech_disconnect(void)
{}

/** The 10 ms a pass-through probe waits pass at once. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** No sysfs tree is published by this model. */
void evdev_sysfs_serio_remove(int port)
{
    (void)port;
}

void evdev_sysfs_serio_add(int port)
{
    (void)port;
}

/** Sleep a competing callback on its own stack until the command is free. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    while (!cond(arg)) {
        park();
    }
    return 0;
}

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    files_port = port;
    files_add = add;
}

void evdev_sysfs_serio_driver(int port, int (*add)(int dir))
{
    if (port == EVDEV_SERIO_PASS_THROUGH) {
        driver_add = add;
        driver_binds += add != NULL;
    }
}

unsigned int elantech_set_rate(unsigned int rate)
{
    return rate;
}

int elantech_packet_size(void)
{
    return 6;
}

int elantech_sysfs_add(int dir)
{
    (void)dir;
    return 0;
}

int alps_poll(uint8_t badbyte, uint8_t *buf)
{
    (void)badbyte;
    (void)buf;
    return -1;
}

/** Record the file asked for; its handle is its place in the record. */
int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)obj;
    if (n_attrs >= ATTRS_MAX) {
        return -ENOMEM;
    }
    attr_made[n_attrs].parent = parent;
    sprintf(attr_made[n_attrs].name, "%s", attr->name);
    attr_made[n_attrs].mode = attr->mode;
    attr_made[n_attrs].show = attr->show;
    attr_made[n_attrs].store = attr->store;
    attr_made[n_attrs].arg = arg;
    n_attrs++;
    return n_attrs;
}

/** The console's blanking is not modelled. */
void console_poke(void)
{}

/* What evdev.c needs: one processor, fixed clocks, nothing to repeat,
 * reads that never sleep, since every open is nonblocking, and calls of
 * x86-64 programs. */

int in_compat_syscall(void)
{
    return 0;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

void input_get_repeat(int *delay_ms, int *period_ms)
{
    *delay_ms = 250;
    *period_ms = 33;
}

/** No keyboard here takes its keys through input.c. */
int input_keyboard_release(struct evdev *dev, unsigned int keycode)
{
    (void)dev;
    (void)keycode;
    return 0;
}

void input_set_repeat(int delay_ms, int period_ms)
{
    (void)delay_ms;
    (void)period_ms;
}

/** No device of a row has keys the console takes. */
void input_console_key(const void *device, unsigned int keycode, int value)
{
    (void)device;
    (void)keycode;
    (void)value;
}

void input_written(uint16_t type, uint16_t code, int32_t value)
{
    (void)type;
    (void)code;
    (void)value;
}

void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy((void *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/* An evdev record as the host's C library lays it out, 24 bytes. */
struct record {
    unsigned long sec;
    unsigned long usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* An open of an event device. */
struct watch {
    fd_entry_t e;
    struct open_file ofd;
    int open;
};

static struct watch pad_w;
static struct watch guest_w;

/* The records the last `take` read. */
static struct record got[128];
static int n_got;

/** Open event device `n`, nonblocking, so that `take` reads what comes
 * after. Returns what `evdev_open` returns. */
static int32_t watch(struct watch *w, int n)
{
    int32_t rc;

    k_memset(w, 0, sizeof(*w));
    w->ofd.status_flags = OFD_NONBLOCK;
    w->e.ofd = &w->ofd;
    rc = n < 0 ? -ENODEV : evdev_open(&w->e, n);
    w->open = rc == 0;
    return rc;
}

/** Close the open `w`, if it is. */
static void unwatch(struct watch *w)
{
    if (w->open) {
        evdev_close(&w->e);
        w->open = 0;
    }
}

/** Read every whole packet of `w` into `got`; returns their records. */
static int take(struct watch *w)
{
    int32_t rc;

    n_got = 0;
    if (!w->open) {
        return 0;
    }
    rc = evdev_read(&w->e, (char __user *)got, sizeof(got));
    n_got = rc > 0 ? rc / (int32_t)sizeof(got[0]) : 0;
    return n_got;
}

/* An event a row expects. */
struct event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/** Whether `got` holds the `n` events of `want` and nothing else. */
static int got_events(const struct event *want, int n)
{
    int i;

    if (n_got != n) {
        printf("trackpoint-test: read %d records, not %d\n", n_got, n);
        for (i = 0; i < n_got; i++) {
            printf("trackpoint-test:   %u %u %d\n", got[i].type, got[i].code, (int)got[i].value);
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (got[i].type != want[i].type || got[i].code != want[i].code ||
            got[i].value != want[i].value) {
            printf("trackpoint-test: record %d is %u %u %d, not %u %u %d\n", i, got[i].type,
                   got[i].code, (int)got[i].value, want[i].type, want[i].code, (int)want[i].value);
            return 0;
        }
    }
    return 1;
}

/** The last value `got` holds for the key `code`, or -1 for none. */
static int key_value(uint16_t code)
{
    int i, v = -1;

    for (i = 0; i < n_got; i++) {
        if (got[i].type == EV_KEY && got[i].code == code) {
            v = got[i].value;
        }
    }
    return v;
}

/* The description of the last device `node_of` found. */
static struct evdev_desc desc;

/** The node of the registered device whose phys is `phys`, with its
 * description in `desc`, or -1 when none is. */
static int node_of(const char *phys)
{
    int n;

    for (n = 0; n < 32; n++) {
        if (evdev_snapshot(n, &desc) == 1 && strcmp(desc.phys, phys) == 0) {
            return n;
        }
    }
    return -1;
}

/** How many registered devices have the name `name`. */
static int count_named(const char *name)
{
    struct evdev_desc d;
    int n, count = 0;

    for (n = 0; n < 32; n++) {
        if (evdev_snapshot(n, &d) == 1 && strcmp(d.name, name) == 0) {
            count++;
        }
    }
    return count;
}

/** Whether bit `code` of the bits at `bits` is set. */
static int has_bit(const uint8_t *bits, unsigned int code)
{
    return (bits[code / 8] >> (code % 8)) & 1;
}

/** Whether `s` begins with `prefix`. */
static int starts_with(const char *s, const char *prefix)
{
    if (!s) {
        return 0;
    }
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/**
 * Take away what the last row registered and put the controller, the
 * touchpad and the guest as the firmware leaves them: the Lenovo's pad,
 * without a pass-through port, and no guest.
 */
static void model_reset(void)
{
    unwatch(&pad_w);
    unwatch(&guest_w);
    guest_forget();
    synaptics_disconnect();
    if (mouse_dev) {
        evdev_unregister(mouse_dev);
        mouse_dev = NULL;
    }
    touchpad = 0;
    state = STATE_ACTIVE;
    bad_in_a_row = 0;
    cmd_busy = 0;
    reconnect_wanted = 0;
    bat_held = 0;
    packet_index = 0;
    k_memset(&guest, 0, sizeof(guest));
    pktcnt = 0;
    sync_losses = 0;
    is_forcepad = 0;
    press = report_press = 0;
    k_memset(&agm, 0, sizeof(agm));
    agm_count = 0;
    k_memset(firmware_id, 0, sizeof(firmware_id));
    variant = 0;
    buttons = 0;

    model_head = model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_plain = 0;
    dev_tp = 0;
    dev_wheel = 0;
    pad_caps = 0xd00123;
    pad_ext = 0x840300;
    pad_ext_0c = 0x02a800;
    pad_ext_10 = 0;
    pad_board = 2848;
    pad_model_low = 0xb1;
    knock_misses = 0;
    arg_for = 0;
    special = 0;
    res_count = 0;
    pad_mode = 0;
    agm_on = 0;
    pad_reporting = 0;
    n_took = 0;

    guest_kind = GUEST_NONE;
    guest_wheel = 0;
    guest_id = 0;
    k_memset(guest_rates, 0, sizeof(guest_rates));
    tp_kind = TP_VARIANT_IBM;
    tp_fw = 0x0e;
    tp_buttons = 0x33;
    tp_por_fails = 0;
    tp_refuse_ext = 0;
    tp_stage = 0;
    g_arg_for = 0;
    guest_reporting = 0;
    n_gtook = 0;
    tp_power_on();

    warnings = 0;
    last_warning = 0;
    last_info = 0;
    files_add = 0;
    files_port = -1;
    driver_add = 0;
    driver_binds = 0;
    n_attrs = 0;
}

/** Give the PS/2 mouse's port the IDs `hid` and PNP0F13, as the firmware
 * of a ThinkPad does, through `i8042_pnp_device`. */
static void give_pnp_id(const char *hid)
{
    const char *ids[2];

    ids[0] = hid;
    ids[1] = "PNP0F13";
    (void)i8042_pnp_device(ids, 2);
}

/** Bring the device up, counting the bytes from here. */
static int bring_up(void)
{
    n_took = 0;
    n_gtook = 0;
    return init_mouse();
}

/** Run IRQ 12 for each of the `n` bytes of `bytes`. */
static void irq_bytes(const uint8_t *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        model_put(bytes[i]);
        mouse_irq_handler(0);
    }
}

/** Run IRQ 12 for the byte `b`. */
static void irq_byte(uint8_t b)
{
    irq_bytes(&b, 1);
}

/* The bytes of the next `pad_packet` that reach the handler, fewer than
 * six for a row that cuts the packet short. */
static int pad_cut = 6;

/**
 * Run IRQ 12 for a packet of the touchpad's own of the new absolute form,
 * as Linux's `synaptics_parse_hw_state` reads it: the place, pressure and
 * width, with no button of byte 0's; `flip` the bits of byte 3
 * that differ from byte 0's, 0x01 for a clickpad's click and 0x02 for the
 * extra buttons; and `extra` the extra buttons' bits, the low two in byte
 * 4 and the next two in byte 5. Only the first `pad_cut` bytes are sent.
 */
static void pad_packet(int x, int y, int z, int w, uint8_t flip, uint8_t extra)
{
    uint8_t p[6];

    p[0] = (uint8_t)(0x80 | ((w & 0x0c) << 2) | ((w & 0x02) << 1));
    p[3] = (uint8_t)(0xC0 | ((w & 0x01) << 2) | ((x >> 8) & 0x10) | ((y >> 7) & 0x20) |
                     ((p[0] & 0x03) ^ flip));
    p[1] = (uint8_t)(((x >> 8) & 0x0f) | ((y >> 4) & 0xf0));
    p[2] = (uint8_t)z;
    p[4] = (uint8_t)((x & 0xfc) | (extra & 0x03));
    p[5] = (uint8_t)((y & 0xfc) | ((extra >> 2) & 0x03));
    irq_bytes(p, pad_cut);
}

/** Run IRQ 12 for a pass-through packet carrying the guest's packet
 * `g0` `g1` `g2` and, for a four-byte guest, `g3`: bytes 1, 4, 5 and 2. */
static void guest_packet(uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3)
{
    uint8_t p[6];

    p[0] = 0x84;
    p[1] = g0;
    p[2] = g3;
    p[3] = 0xC4;
    p[4] = g1;
    p[5] = g2;
    irq_bytes(p, 6);
}

/* The bytes a row expects the touchpad, or the guest, to take. */
static uint8_t want[TOOK_MAX];
static int n_want;

/** Expect the byte `b`. */
static void expect(uint8_t b)
{
    if (n_want < TOOK_MAX) {
        want[n_want] = b;
    }
    n_want++;
}

/** Expect the `n` bytes of `bytes`. */
static void expect_n(const uint8_t *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        expect(bytes[i]);
    }
}

/** Expect the touchpad's request `req`, sliced. */
static void expect_sliced(uint8_t req)
{
    int i;

    expect(0xE6);
    for (i = 6; i >= 0; i -= 2) {
        expect(0xE8);
        expect((uint8_t)((req >> i) & 3u));
    }
}

/** Expect the query `q`. */
static void expect_query(uint8_t q)
{
    expect_sliced(q);
    expect(0xE9);
}

/** Expect the mode byte `m` set. */
static void expect_mode(uint8_t m)
{
    expect_sliced(m);
    expect(0xF3);
    expect(0x14);
}

/** Expect Linux's `synaptics_detect`, its knock. */
static void expect_knock(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        expect(0xE8);
        expect(0x00);
    }
    expect(0xE9);
}

/** Expect what `synaptics_query_hardware` asks, then `synaptics_set_mode`
 * and `psmouse_initialize`, for the Lenovo's pad. */
static void expect_queries_and_mode(void)
{
    static const uint8_t queries[] = {0x00, 0x03, 0x0a, 0x01, 0x02, 0x09, 0x0c, 0x08, 0x0d, 0x0f};
    unsigned i;

    for (i = 0; i < sizeof(queries); i++) {
        expect_query(queries[i]);
    }
    expect_mode(0xC5);
    expect_sliced(0x03); /* the advanced gesture mode */
    expect(0xF3);
    expect(0xC8);
    expect_mode(0xC5); /* psmouse_initialize: the rate, 200 dpi, 1:1 */
    expect(0xE8);
    expect(0x03);
    expect(0xE6);
}

/** Whether the `n` bytes at `bytes` are the bytes expected; prints the
 * first that differs. */
static int bytes_expected(const char *who, const uint8_t *bytes, int n)
{
    int i;

    if (n != n_want) {
        printf("trackpoint-test: the %s took %d bytes, not %d\n", who, n, n_want);
    }
    for (i = 0; i < n && i < n_want && i < TOOK_MAX; i++) {
        if (bytes[i] != want[i]) {
            printf("trackpoint-test: byte %d to the %s is 0x%02x, not 0x%02x\n", i, who, bytes[i],
                   want[i]);
            return 0;
        }
    }
    return n == n_want;
}

/** Whether the guest took the bytes expected. */
static int guest_took_expected(void)
{
    return bytes_expected("guest", gtook, n_gtook);
}

/** Whether the touchpad took the bytes expected. */
static int pad_took_expected(void)
{
    return bytes_expected("touchpad", took, n_took);
}

/* Linux's psmouse_probe and the start of psmouse_extensions on a
 * pass-through port: the ID, the reset to the defaults, the defaults again
 * before the ALPS probe it skips there, and the TrackPoint's read-ID. */
static const uint8_t probe_start[] = {0xF2, 0xF6, 0xF6, 0xE1};

/* Linux's psmouse_initialize and psmouse_activate: 100 samples a second,
 * 200 dpi, 1:1, and reporting. */
static const uint8_t initialize_activate[] = {0xF3, 100, 0xE8, 0x03, 0xE6, 0xF4};

/* After a mouse refused the read-ID: the defaults, the reset, and the
 * IntelliMouse Explorer's knock behind the IntelliMouse's. */
static const uint8_t mouse_knocks[] = {0xF6, 0xFF, 0xF3, 200,  0xF3, 100,  0xF3, 80,
                                       0xF2, 0xF3, 200,  0xF3, 200,  0xF3, 80,   0xF2};

/* The IntelliMouse's knock alone, and the Explorer 4.0's horizontal wheel. */
static const uint8_t imps_knock[] = {0xF3, 200, 0xF3, 100, 0xF3, 80, 0xF2};
static const uint8_t hwheel_rates[] = {0xF3, 200, 0xF3, 80, 0xF3, 40};

/* The settings' locations and power-on values in the order Linux's
 * `trackpoint_sync` writes them, the eleven written whole. */
static const uint8_t sync_writes[11][2] = {{0x4A, 0x80}, {0x4D, 0x06}, {0x60, 0x61}, {0x57, 0x0A},
                                           {0x58, 0xFF}, {0x59, 0x14}, {0x5C, 0x08}, {0x5A, 0xFF},
                                           {0x5E, 0x26}, {0x5D, 0x87}, {0x5F, 0x05}};

/** Expect a read of the TrackPoint's location `loc`. */
static void expect_tp_read(uint8_t loc)
{
    expect(0xE2);
    expect(loc);
}

/** Expect a write of `val` to the TrackPoint's location `loc`. */
static void expect_tp_write(uint8_t loc, uint8_t val)
{
    expect(0xE2);
    expect(0x81);
    expect(loc);
    expect(val);
}

/** Expect a toggle of the bits `mask` of the TrackPoint's location `loc`. */
static void expect_tp_toggle(uint8_t loc, uint8_t mask)
{
    expect(0xE2);
    expect(0x47);
    expect(loc);
    expect(mask);
}

/** Expect Linux's `trackpoint_sync(psmouse, false)` on an IBM stick whose
 * RAM is at its power-on values, with `sensitivity` the value kept. */
static void expect_ibm_sync(uint8_t sensitivity)
{
    int i;

    expect_tp_read(0x2D); /* two-handed */
    expect_tp_read(0x20); /* source tag */
    expect_tp_read(0x23); /* middle button */
    for (i = 0; i < 11; i++) {
        expect_tp_write(sync_writes[i][0], i == 0 ? sensitivity : sync_writes[i][1]);
    }
    expect_tp_read(0x2C); /* press_to_select */
    expect_tp_read(0x2D); /* skipback */
    expect_tp_read(0x23); /* ext_dev */
}

/** The file `name` the stick's driver added, or -1. */
static int attr_named(const char *name)
{
    int i;

    for (i = 0; i < n_attrs; i++) {
        if (strcmp(attr_made[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* What the last `show_file` read. */
static char shown[32];

/** Read the file `name` into `shown`; returns the show's count. */
static int32_t show_file(const char *name)
{
    int i = attr_named(name);
    int32_t n;

    shown[0] = '\0';
    if (i < 0) {
        return -ENOENT;
    }
    n = attr_made[i].show(NULL, attr_made[i].arg, shown, sizeof(shown) - 1);
    shown[n > 0 && n < (int32_t)sizeof(shown) ? n : 0] = '\0';
    return n;
}

/** Write the `len` bytes of `text` to the file `name`, counting the bytes
 * the touchpad and the guest take from here; returns the store's result. */
static int32_t store_file(const char *name, const char *text, uint32_t len)
{
    int i = attr_named(name);

    n_took = 0;
    n_gtook = 0;
    if (i < 0) {
        return -ENOENT;
    }
    return attr_made[i].store(NULL, attr_made[i].arg, text, len);
}

/** The PNP IDs of the 8042's devices as Linux's pnpacpi and i8042 take
 * them, and their match against a quirk table. */
static void pnp_rows(void)
{
    static const char *const lenovo[] = {"LEN0036", "PNP0F13"};
    static const char *const kbd[] = {"LEN0071", "PNP0303"};
    static const char *const synaptics[] = {"SYN1219", "SYN1200", "SYN0002", "PNP0F13"};
    static const char *const forms[] = {"MSFT0001", "len0036", "LEN003", "PNP0F13"};
    static const char *const twice[] = {"PNP0F13", "PNP0F13"};
    static const char *const battery[] = {"PNP0C0A"};
    static const char *const ec[] = {"ACPI0003", "PNP0C09"};
    static const char *const fujitsu[] = {"FJC6001"};
    static const char *const len0036[] = {"LEN0036", 0};
    static const char *const len003[] = {"LEN003", 0};
    static const char *const mixed[] = {"pnp0F13", 0};
    static const char *const len2006[] = {"LEN2006", 0};

    model_reset();
    CHECK("a ThinkPad's touchpad IDs name the PS/2 mouse's port",
          i8042_pnp_device(lenovo, 2) == I8042_AUX_PORT);
    CHECK("its firmware ID is Linux's: \"PNP:\" and each ID, the digits in lower case",
          strcmp(i8042_firmware_id(I8042_AUX_PORT), "PNP: LEN0036 PNP0f13") == 0);
    CHECK("the keyboard's port has none yet", i8042_firmware_id(I8042_KBD_PORT)[0] == '\0');
    CHECK("a keyboard's _HID and _CID name the keyboard's port",
          i8042_pnp_device(kbd, 2) == I8042_KBD_PORT &&
              strcmp(i8042_firmware_id(I8042_KBD_PORT), "PNP: LEN0071 PNP0303") == 0);
    CHECK("the mouse's ID stays",
          strcmp(i8042_firmware_id(I8042_AUX_PORT), "PNP: LEN0036 PNP0f13") == 0);
    CHECK("every _CID is kept, in order", i8042_pnp_device(synaptics, 4) == I8042_AUX_PORT &&
                                              strcmp(i8042_firmware_id(I8042_AUX_PORT),
                                                     "PNP: SYN1219 SYN1200 SYN0002 PNP0f13") == 0);
    CHECK("IDs not of three capitals and four hexadecimal digits are left out",
          i8042_pnp_device(forms, 4) == I8042_AUX_PORT &&
              strcmp(i8042_firmware_id(I8042_AUX_PORT), "PNP: PNP0f13") == 0);
    CHECK("an ID the same as the first is left out",
          i8042_pnp_device(twice, 2) == I8042_AUX_PORT &&
              strcmp(i8042_firmware_id(I8042_AUX_PORT), "PNP: PNP0f13") == 0);
    CHECK("a battery is neither port's", i8042_pnp_device(battery, 1) == -1);
    CHECK("an embedded controller is neither port's", i8042_pnp_device(ec, 2) == -1);
    CHECK("Fujitsu's touchpad is in Linux's aux table",
          i8042_pnp_device(fujitsu, 1) == I8042_AUX_PORT);

    (void)i8042_pnp_device(lenovo, 2);
    CHECK("a table's ID matches", i8042_matches_pnp_id(I8042_AUX_PORT, len0036));
    CHECK("a part of an ID does not", !i8042_matches_pnp_id(I8042_AUX_PORT, len003));
    CHECK("case does not count", i8042_matches_pnp_id(I8042_AUX_PORT, mixed));
    CHECK("another ID does not match", !i8042_matches_pnp_id(I8042_AUX_PORT, len2006));
    k_memset(firmware_id, 0, sizeof(firmware_id));
    CHECK("a port with no firmware ID matches nothing",
          !i8042_matches_pnp_id(I8042_AUX_PORT, len0036));
}

/** Each pad of Linux's `min_max_pnpid_table` gets its coordinates, within
 * its board IDs, and the others keep theirs. */
static void min_max_rows(void)
{
    const struct min_max_quirk *q;
    struct input_absinfo ax, ay;
    char what[160];
    char upper[8];
    int i, j, all = 1;

    for (q = min_max_quirks; q->ids; q++) {
        for (i = 0; q->ids[i]; i++) {
            model_reset();
            for (j = 0; j < 7; j++) {
                upper[j] = (char)k_toupper(q->ids[i][j]);
            }
            upper[7] = '\0';
            give_pnp_id(upper);
            pad_board = q->board_max ? q->board_max : 2848;
            /* The second LEN2006 row is for every board but the first's. */
            if (q > min_max_quirks && q[-1].ids == q->ids) {
                pad_board = 2848;
            }
            if (bring_up() != 0 || evdev_get_abs(pad, ABS_X, &ax) < 0 ||
                evdev_get_abs(pad, ABS_Y, &ay) < 0) {
                sprintf(what, "%s on board %u is brought up", upper, pad_board);
                CHECK(what, 0);
                all = 0;
                continue;
            }
            if (info.x_min != q->x_min || info.x_max != q->x_max || info.y_min != q->y_min ||
                info.y_max != q->y_max || ax.minimum != (int32_t)q->x_min ||
                ax.maximum != (int32_t)q->x_max) {
                sprintf(what, "%s on board %u gets x %u..%u, y %u..%u", upper, pad_board, q->x_min,
                        q->x_max, q->y_min, q->y_max);
                CHECK(what, 0);
                all = 0;
            }
        }
    }
    CHECK("every pad of min_max_pnpid_table gets its coordinates", all);

    model_reset();
    give_pnp_id("LEN0034");
    pad_board = 2962;
    CHECK("LEN0034 past board 2961 keeps what it reports",
          bring_up() == 0 && info.x_min == 1314 && info.x_max == 5670 && info.y_min == 1098 &&
              info.y_max == 4794);
    model_reset();
    give_pnp_id("LEN0036");
    pad_board = 2961;
    CHECK("LEN0036 on board 2961 gets LEN0034's", bring_up() == 0 && info.x_max == 5112);
    model_reset();
    give_pnp_id("LEN2006");
    pad_board = 2691;
    CHECK("LEN2006 on board 2691 gets its own row", bring_up() == 0 && info.x_max == 5045);
    model_reset();
    give_pnp_id("LEN0035");
    CHECK("a pad of no row keeps what it reports",
          bring_up() == 0 && info.x_min == 1314 && info.x_max == 5670);
}

/** INPUT_PROP_TOPBUTTONPAD on each clickpad of Linux's
 * `topbuttonpad_pnp_ids` without the stick's buttons, and nowhere else. */
static void topbutton_rows(void)
{
    char what[96];
    char upper[8];
    int i, j, n, all = 1;

    for (i = 0; topbuttonpad_pnp_ids[i]; i++) {
        model_reset();
        for (j = 0; j < 7; j++) {
            upper[j] = (char)k_toupper(topbuttonpad_pnp_ids[i][j]);
        }
        upper[7] = '\0';
        give_pnp_id(upper);
        pad_ext_0c |= PAD_CLICKPAD;
        n = bring_up() == 0 ? node_of("isa0060/serio1/input0") : -1;
        if (n < 0 || !has_bit(desc.prop, INPUT_PROP_TOPBUTTONPAD) ||
            !has_bit(desc.prop, INPUT_PROP_BUTTONPAD)) {
            sprintf(what, "%s is a clickpad with buttons at the top", upper);
            CHECK(what, 0);
            all = 0;
        }
    }
    CHECK("every clickpad of topbuttonpad_pnp_ids has its buttons at the top", all);

    model_reset();
    give_pnp_id("LEN0036");
    pad_ext_0c |= PAD_CLICKPAD;
    pad_ext_10 = PAD_STICK_10;
    n = bring_up() == 0 ? node_of("isa0060/serio1/input0") : -1;
    CHECK("one whose stick's buttons are wired to it has none",
          n >= 0 && !has_bit(desc.prop, INPUT_PROP_TOPBUTTONPAD));
    model_reset();
    give_pnp_id("LEN0036");
    n = bring_up() == 0 ? node_of("isa0060/serio1/input0") : -1;
    CHECK("a listed pad that is no clickpad has none",
          n >= 0 && !has_bit(desc.prop, INPUT_PROP_TOPBUTTONPAD));
    model_reset();
    give_pnp_id("LEN0048");
    pad_ext_0c |= PAD_CLICKPAD;
    n = bring_up() == 0 ? node_of("isa0060/serio1/input0") : -1;
    CHECK("a clickpad not listed has none", n >= 0 &&
                                                !has_bit(desc.prop, INPUT_PROP_TOPBUTTONPAD) &&
                                                has_bit(desc.prop, INPUT_PROP_BUTTONPAD));
}

/** A ForcePad of Linux's `forcepad_pnp_ids` clicks only for one finger
 * pressed 50 ms, and a clickpad not listed at once. */
static void forcepad_rows(void)
{
    char what[96];
    int i;

    for (i = 0; forcepad_pnp_ids[i]; i++) {
        model_reset();
        give_pnp_id(forcepad_pnp_ids[i]);
        pad_ext_0c |= PAD_CLICKPAD;
        CHECK("the ForcePad is brought up", bring_up() == 0 && is_forcepad);
        watch(&pad_w, node_of("isa0060/serio1/input0"));
        pad_packet(3000, 3000, 50, 8, 0x01, 0);
        take(&pad_w);
        sprintf(what, "%s: a press is no click at once", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) != 1);
        kernel_ticks += MS_TO_TICKS(FORCEPAD_PRESS_MS);
        pad_packet(3000, 3000, 50, 8, 0x01, 0);
        take(&pad_w);
        sprintf(what, "%s: nor at 50 ms", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) != 1);
        kernel_ticks += 1;
        pad_packet(3000, 3000, 50, 8, 0x01, 0);
        take(&pad_w);
        sprintf(what, "%s: a press held past 50 ms is the left button", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) == 1);
        pad_packet(3000, 3000, 50, 8, 0x00, 0);
        take(&pad_w);
        sprintf(what, "%s: the click stays while the finger touches", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) != 0);
        pad_packet(0, 0, 0, 0, 0x00, 0);
        take(&pad_w);
        sprintf(what, "%s: and ends when it lifts", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) == 0);
        pad_packet(3000, 3000, 50, 0, 0x01, 0);
        kernel_ticks += MS_TO_TICKS(100);
        pad_packet(3000, 3000, 50, 0, 0x01, 0);
        take(&pad_w);
        sprintf(what, "%s: two fingers' press is no click", forcepad_pnp_ids[i]);
        CHECK(what, key_value(BTN_LEFT) != 1);
    }

    model_reset();
    give_pnp_id("LEN0036");
    pad_ext_0c |= PAD_CLICKPAD;
    CHECK("a clickpad not listed is no ForcePad", bring_up() == 0 && !is_forcepad);
    watch(&pad_w, node_of("isa0060/serio1/input0"));
    pad_packet(3000, 3000, 50, 8, 0x01, 0);
    take(&pad_w);
    CHECK("and clicks at once", key_value(BTN_LEFT) == 1);
}

/** What a pad that offers SMBus logs: that Linux drives it on SMBus, for
 * each of Linux's `topbuttonpad_pnp_ids` and `smbus_pnp_ids`, or only that
 * it can, and nothing for a ForcePad. */
static void smbus_rows(void)
{
    const char *const *tables[2];
    char what[96];
    char upper[8];
    int t, i, j, all = 1;

    tables[0] = topbuttonpad_pnp_ids;
    tables[1] = smbus_pnp_ids;
    for (t = 0; t < 2; t++) {
        for (i = 0; tables[t][i]; i++) {
            model_reset();
            for (j = 0; j < 7; j++) {
                upper[j] = (char)k_toupper(tables[t][i][j]);
            }
            upper[7] = '\0';
            give_pnp_id(upper);
            pad_ext_0c |= PAD_INTERTOUCH;
            if (bring_up() != 0 ||
                !starts_with(last_warning,
                             "smbus: the touchpad (%s) is one Linux drives on SMBus")) {
                sprintf(what, "%s is one Linux drives on SMBus", upper);
                CHECK(what, 0);
                all = 0;
            }
        }
    }
    CHECK("every pad of topbuttonpad_pnp_ids and smbus_pnp_ids is one Linux drives on SMBus", all);

    model_reset();
    give_pnp_id("LEN0030");
    pad_ext_0c |= PAD_INTERTOUCH;
    CHECK("another pad offering SMBus says only that it can",
          bring_up() == 0 &&
              starts_with(last_warning, "smbus: the touchpad (%s) says it can support"));
    CHECK("and stays on PS/2", touchpad == 1 && pad_reporting);
    model_reset();
    give_pnp_id("SYN300D");
    pad_ext_0c |= PAD_INTERTOUCH;
    CHECK("a ForcePad offering SMBus logs nothing", bring_up() == 0 && warnings == 0);
    model_reset();
    give_pnp_id("LEN0048");
    CHECK("a listed pad that does not offer SMBus logs nothing", bring_up() == 0 && warnings == 0);
}

/* The vendors Linux's `trackpoint_variants` names, by kind. */
static const char *const tp_vendors[] = {"",    "IBM",           "ALPS",     "Elan",
                                         "NXP", "JYT_Synaptics", "Synaptics"};

/** A TrackPoint of `kind` brought up: the bytes Linux sends it, and its
 * event device. Returns the device's node. */
static int kind_bringup(int kind)
{
    const char *v = tp_vendors[kind];
    char what[128];
    char name[48];
    int n;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_kind = (uint8_t)kind;
    sprintf(what, "%s: the bring-up succeeds", v);
    CHECK(what, bring_up() == 0);
    n_want = 0;
    expect_n(probe_start, 4);
    if (kind == TP_VARIANT_IBM) {
        expect_tp_read(0x4B); /* the extended button query */
        expect(0xE2);
        expect(0x7F); /* the power-on reset, which passed */
    } else {
        expect_tp_write(0x4A, 0x80); /* sensitivity */
        expect_tp_read(0x2C);        /* press_to_select */
    }
    expect_n(initialize_activate, 6);
    sprintf(what, "%s: the stick takes Linux's bytes, in Linux's order", v);
    CHECK(what, guest_took_expected());
    sprintf(name, "TPPS/2 %s TrackPoint", v);
    n = node_of("synaptics-pt/serio0/input0");
    sprintf(what, "%s: its device is \"%s\" at synaptics-pt/serio0/input0", v, name);
    CHECK(what, n >= 0 && strcmp(desc.name, name) == 0);
    sprintf(what, "%s: bus i8042, vendor 2, product 10, Linux's PSMOUSE_TRACKPOINT", v);
    CHECK(what,
          n >= 0 && desc.id[0] == EVDEV_BUS_I8042 && desc.id[1] == 0x0002 && desc.id[2] == 10);
    sprintf(what, "%s: a pointer and a pointing stick, three buttons and two axes", v);
    CHECK(what, n >= 0 && has_bit(desc.prop, INPUT_PROP_POINTER) &&
                    has_bit(desc.prop, INPUT_PROP_POINTING_STICK) && has_bit(desc.key, BTN_LEFT) &&
                    has_bit(desc.key, BTN_RIGHT) && has_bit(desc.key, BTN_MIDDLE) &&
                    has_bit(desc.rel, REL_X) && has_bit(desc.rel, REL_Y) &&
                    !has_bit(desc.rel, REL_WHEEL));
    sprintf(what, "%s: the pad carries three-byte packets and both report", v);
    CHECK(what,
          (pad_mode & MODE_FOUR_BYTE) == 0 && pad_reporting && guest_reporting && guest.active);
    return n;
}

/** The files of a TrackPoint of `kind` on node `n`, and its packet. */
static void kind_files(int kind, int n)
{
    static const char *const ibm_files[] = {
        "sensitivity", "speed", "inertia", "reach",      "draghys",         "mindrag",  "thresh",
        "upthresh",    "ztime", "jenks",   "drift_time", "press_to_select", "skipback", "ext_dev"};
    static const struct event stick[] = {
        {EV_KEY, BTN_MIDDLE, 1}, {EV_REL, REL_X, -2}, {EV_REL, REL_Y, -3}, {EV_SYN, SYN_REPORT, 0}};
    const char *v = tp_vendors[kind];
    char what[128];
    int i, ok;

    sprintf(what, "%s: its files are given to the pass-through port", v);
    CHECK(what, files_port == EVDEV_SERIO_PASS_THROUGH && files_add == trackpoint_sysfs_add);
    sprintf(what, "%s: beside psmouse's, which are the port's driver's", v);
    CHECK(what, driver_add == guest_files);
    n_attrs = 0;
    ok = trackpoint_sysfs_add(7) == 0;
    if (kind == TP_VARIANT_IBM) {
        ok = ok && n_attrs == 14;
        for (i = 0; ok && i < 14; i++) {
            ok = strcmp(attr_made[i].name, ibm_files[i]) == 0 && attr_made[i].mode == 0644u &&
                 attr_made[i].parent == 7;
        }
        sprintf(what, "%s: all fourteen of Linux's files, root's to write", v);
    } else {
        ok = ok && n_attrs == 2 && strcmp(attr_made[0].name, "sensitivity") == 0 &&
             strcmp(attr_made[1].name, "press_to_select") == 0 && attr_made[0].mode == 0644u;
        sprintf(what, "%s: sensitivity and press_to_select alone, as Linux's", v);
    }
    CHECK(what, ok);
    watch(&guest_w, n);
    guest_packet(0x1C, 0xFE, 0x03, 0x00);
    take(&guest_w);
    sprintf(what, "%s: its packet is the middle button, 2 left and 3 away from the user", v);
    CHECK(what, got_events(stick, 4));
    sprintf(what, "%s: every byte is read", v);
    CHECK(what, model_count == 0);
}

/** A TrackPoint of each kind on the pass-through port. */
static void kind_rows(void)
{
    int kind;

    for (kind = TP_VARIANT_IBM; kind <= TP_VARIANT_SYNAPTICS; kind++) {
        kind_files(kind, kind_bringup(kind));
    }
}

/** An IBM stick's buttons and power-on reset, the newer kinds' press to
 * select, and a kind Linux does not know. */
static void ibm_rows(void)
{
    int n, i;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_buttons = 0x22;
    tp_power_on();
    CHECK("an IBM stick of two buttons is brought up", bring_up() == 0);
    n = node_of("synaptics-pt/serio0/input0");
    CHECK("its device has no middle button",
          n >= 0 && !has_bit(desc.key, BTN_MIDDLE) && has_bit(desc.key, BTN_RIGHT));

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_buttons = 0x00;
    tp_power_on();
    CHECK("an IBM stick answering 0 buttons is brought up", bring_up() == 0);
    CHECK("it is taken for three, with Linux's warning",
          buttons == 0x33 &&
              starts_with(last_warning, "buttons: 0 in the extended button data, assuming 3"));

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_refuse_ext = 1;
    CHECK("an IBM stick refusing the button query is brought up", bring_up() == 0);
    CHECK("it is taken for three, with Linux's warning",
          buttons == 0x33 && starts_with(last_warning, "buttons: no extended button data"));

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_por_fails = 1;
    tp_ram[0x2D] = 0x01; /* two-handed mode on */
    CHECK("an IBM stick whose reset fails is brought up", bring_up() == 0);
    n_want = 0;
    expect_n(probe_start, 4);
    expect_tp_read(0x4B);
    expect(0xE2);
    expect(0x7F);
    expect_tp_read(0x2D);
    expect_tp_toggle(0x2D, 0x01); /* two-handed mode turned off */
    expect_tp_read(0x20);
    expect_tp_read(0x23);
    for (i = 0; i < 11; i++) {
        expect_tp_write(sync_writes[i][0], sync_writes[i][1]);
    }
    expect_tp_read(0x2C);
    expect_tp_read(0x2D);
    expect_tp_read(0x23);
    expect_n(initialize_activate, 6);
    CHECK("each setting is written, as Linux writes a stick in an unknown state",
          guest_took_expected());
    CHECK("and two-handed mode is off", (tp_ram[0x2D] & 0x01) == 0);

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_kind = TP_VARIANT_ELAN;
    tp_ram[0x2C] = 0x01; /* press to select on */
    CHECK("an Elan stick with press to select on is brought up", bring_up() == 0);
    n_want = 0;
    expect_n(probe_start, 4);
    expect_tp_write(0x4A, 0x80);
    expect_tp_read(0x2C);
    expect_tp_toggle(0x2C, 0x01);
    expect_n(initialize_activate, 6);
    CHECK("it is turned off, as the newer kinds are set", guest_took_expected());

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_kind = 0x07;
    CHECK("a stick of a kind Linux does not know is brought up", bring_up() == 0);
    n = node_of("synaptics-pt/serio0/input0");
    CHECK("as a mouse", n >= 0 && strcmp(desc.name, "PS/2 Generic Mouse") == 0 && desc.id[2] == 1);
}

/** The bytes a pass-through byte goes in: the request sliced and the rate
 * 0x28, with the touchpad disabled first. */
static void framing_rows(void)
{
    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("a pad with an IBM stick is brought up", bring_up() == 0);
    n_took = 0;
    n_gtook = 0;
    CHECK("a guest's settings change begins", ps2mouse_guest_change_begin() == &synaptics_pt);
    n_want = 0;
    expect(0xF5); /* the touchpad disabled, psmouse_deactivate */
    expect_sliced(0xF5);
    expect(0xF3);
    expect(0x28);
    CHECK("the touchpad is disabled, then the guest's 0xF5 sliced and sent by the rate 0x28",
          pad_took_expected());
    ps2mouse_guest_change_end();
    CHECK("and both enabled again, the guest first", n_gtook == 2 && gtook[1] == 0xF4 &&
                                                         took[n_took - 1] == 0xF4 &&
                                                         pad_reporting && guest_reporting);
    CHECK("every byte is read", model_count == 0);

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    CHECK("a pad with nothing behind its port is brought up", bring_up() == 0);
    CHECK("its port took the ID request alone", n_gtook == 1 && gtook[0] == 0xF2);
    CHECK("no guest is registered and the pad reports",
          node_of("synaptics-pt/serio0/input0") < 0 && pad_reporting && !guest.active);
    CHECK("the pad has three-byte packets", (pad_mode & MODE_FOUR_BYTE) == 0);
}

/** A mouse on the pass-through port, of each protocol Linux tries there,
 * and its packets. */
static void mouse_rows(void)
{
    static const struct event wheel[] = {{EV_REL, REL_WHEEL, 1}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event hwheel[] = {{EV_REL, REL_HWHEEL, -1}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event side[] = {{EV_KEY, BTN_SIDE, 1}, {EV_SYN, SYN_REPORT, 0}};
    int n;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_MOUSE;
    CHECK("a pad with a mouse behind it is brought up", bring_up() == 0);
    n_want = 0;
    expect_n(probe_start, 4);
    expect_n(mouse_knocks, sizeof(mouse_knocks));
    expect_n(imps_knock, sizeof(imps_knock));
    expect_n(initialize_activate, 6);
    CHECK("a plain mouse takes Linux's probe: ID, defaults, read-ID refused, reset, knocks",
          guest_took_expected());
    n = node_of("synaptics-pt/serio0/input0");
    CHECK("it is \"PS/2 Generic Mouse\", product 1, three buttons",
          n >= 0 && strcmp(desc.name, "PS/2 Generic Mouse") == 0 && desc.id[2] == 1 &&
              has_bit(desc.key, BTN_MIDDLE) && !has_bit(desc.prop, INPUT_PROP_POINTING_STICK));
    CHECK("it has no files of its own", files_port == EVDEV_SERIO_PASS_THROUGH && files_add == 0);
    CHECK("but psmouse's", driver_add == guest_files);
    CHECK("three-byte packets", (pad_mode & MODE_FOUR_BYTE) == 0);

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_MOUSE;
    guest_wheel = 3;
    CHECK("a pad with a wheel mouse behind it is brought up", bring_up() == 0);
    n = node_of("synaptics-pt/serio0/input0");
    CHECK("it is \"ImPS/2 Generic Wheel Mouse\", product 5, with a wheel",
          n >= 0 && strcmp(desc.name, "ImPS/2 Generic Wheel Mouse") == 0 && desc.id[2] == 5 &&
              has_bit(desc.rel, REL_WHEEL));
    CHECK("the pad carries four-byte packets", (pad_mode & MODE_FOUR_BYTE) != 0);
    watch(&guest_w, n);
    guest_packet(0x08, 0x00, 0x00, 0xFF);
    take(&guest_w);
    CHECK("the wheel's byte comes in the pass-through packet's byte 2", got_events(wheel, 2));

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_MOUSE;
    guest_wheel = 4;
    CHECK("a pad with an Explorer behind it is brought up", bring_up() == 0);
    n_want = 0;
    expect_n(probe_start, 4);
    expect_n(mouse_knocks, sizeof(mouse_knocks));
    expect_n(hwheel_rates, sizeof(hwheel_rates));
    expect_n(initialize_activate, 6);
    CHECK("an Explorer takes the knocks and the horizontal wheel's rates", guest_took_expected());
    n = node_of("synaptics-pt/serio0/input0");
    CHECK("it is \"ImExPS/2 Generic Explorer Mouse\", product 6",
          n >= 0 && strcmp(desc.name, "ImExPS/2 Generic Explorer Mouse") == 0 && desc.id[2] == 6 &&
              has_bit(desc.rel, REL_HWHEEL) && has_bit(desc.key, BTN_SIDE));
    watch(&guest_w, n);
    guest_packet(0x08, 0x00, 0x00, 0x41);
    take(&guest_w);
    CHECK("its horizontal wheel", got_events(hwheel, 2));
    guest_packet(0x08, 0x00, 0x00, 0x10);
    take(&guest_w);
    CHECK("its side button", got_events(side, 2));
}

/** A TrackPoint's packets through the pad, and the stick's buttons the pad
 * gives out of band. */
static void packet_rows(void)
{
    static const struct event move[] = {
        {EV_KEY, BTN_LEFT, 1}, {EV_REL, REL_X, 5}, {EV_REL, REL_Y, 5}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event release[] = {{EV_KEY, BTN_LEFT, 0}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event left[] = {{EV_KEY, BTN_LEFT, 1}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event held[] = {{EV_REL, REL_X, 3}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event right[] = {
        {EV_KEY, BTN_LEFT, 0}, {EV_KEY, BTN_RIGHT, 1}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event middle[] = {
        {EV_KEY, BTN_RIGHT, 0}, {EV_KEY, BTN_MIDDLE, 1}, {EV_SYN, SYN_REPORT, 0}};
    struct mouse_packet queued[16];
    int n;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("a pad with an IBM stick is brought up", bring_up() == 0);
    n = node_of("synaptics-pt/serio0/input0");
    watch(&guest_w, n);
    watch(&pad_w, node_of("isa0060/serio1/input0"));
    guest_packet(0x29, 0x05, 0xFB, 0x00);
    take(&guest_w);
    CHECK("the stick's packet in bytes 1, 4 and 5: left, 5 right and 5 toward the user",
          got_events(move, 4));
    take(&pad_w);
    CHECK("the pad reports nothing of it", n_got == 0);
    guest_packet(0x08, 0x00, 0x00, 0x00);
    take(&guest_w);
    CHECK("and the release", got_events(release, 2));

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    pad_ext |= PAD_THREE_EXTRA;
    pad_ext_10 = PAD_STICK_10;
    guest_kind = GUEST_TP;
    CHECK("a pad that wires the stick's buttons to itself is brought up", bring_up() == 0);
    n = node_of("synaptics-pt/serio0/input0");
    watch(&guest_w, n);
    watch(&pad_w, node_of("isa0060/serio1/input0"));
    pad_packet(0, 0, 0, 0, 0x02, 0x01);
    take(&guest_w);
    CHECK("the pad's extra button 0 is the stick's left, reported on the stick at once",
          got_events(left, 2));
    take(&pad_w);
    CHECK("and not on the pad", key_value(BTN_LEFT) != 1 && key_value(BTN_0) < 0);
    while (mouse_read_packets(queued, 16) > 0) {
    }
    pad_packet(3000, 3000, 50, 8, 0x02, 0x01);
    take(&guest_w);
    CHECK("the same buttons in the pad's next packet, a touch, report nothing on the stick",
          n_got == 0);
    n = mouse_read_packets(queued, 16);
    CHECK("and the mouse queue has the pad's packet alone", n == 1 && queued[0].buttons == 0);
    guest_packet(0x08, 0x03, 0x00, 0x00);
    take(&guest_w);
    CHECK("the stick's next packet holds it", got_events(held, 2));
    pad_packet(0, 0, 0, 0, 0x02, 0x04);
    take(&guest_w);
    CHECK("the extra button 2 is the stick's right", got_events(right, 3));
    pad_packet(0, 0, 0, 0, 0x02, 0x02);
    take(&guest_w);
    CHECK("and 1 its middle", got_events(middle, 3));
}

/** The stick's files read and written as Linux's psmouse and trackpoint.c
 * have them. */
static void file_rows(void)
{
    static const char nul[] = {'5', '\0', 'x', 'y'};

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("a pad with an IBM stick is brought up", bring_up() == 0);
    n_attrs = 0;
    (void)trackpoint_sysfs_add(7);
    CHECK("sensitivity reads 128 and a newline",
          show_file("sensitivity") == 4 && strcmp(shown, "128\n") == 0);
    CHECK("speed reads 97", show_file("speed") == 3 && strcmp(shown, "97\n") == 0);
    CHECK("press_to_select reads 0",
          show_file("press_to_select") == 2 && strcmp(shown, "0\n") == 0);
    CHECK("ext_dev reads 1, its bit turned over",
          show_file("ext_dev") == 2 && strcmp(shown, "1\n") == 0);

    CHECK("a write of 200 to sensitivity takes its count",
          store_file("sensitivity", "200\n", 4) == 4);
    n_want = 0;
    expect(0xF5);
    expect_tp_write(0x4A, 200);
    expect(0xF4);
    CHECK("the stick disabled, the value written, the stick enabled", guest_took_expected());
    CHECK("the pad disabled before and enabled after",
          n_took > 0 && took[0] == 0xF5 && took[n_took - 1] == 0xF4 && pad_reporting);
    CHECK("the stick has it and the file reads it",
          tp_ram[0x4A] == 200 && show_file("sensitivity") == 4 && strcmp(shown, "200\n") == 0);
    CHECK("every byte is read", model_count == 0);

    CHECK("256 is out of range", store_file("sensitivity", "256", 3) == -ERANGE);
    CHECK("with both disabled and enabled again, as Linux's helper does",
          n_gtook == 2 && gtook[0] == 0xF5 && gtook[1] == 0xF4);
    CHECK("and the value kept", tp_ram[0x4A] == 200 && show_file("sensitivity") == 4);
    CHECK("a word is no number", store_file("sensitivity", "abc", 3) == -EINVAL);
    CHECK("nor is nothing", store_file("sensitivity", "", 0) == -EINVAL);
    CHECK("nor a number and two newlines", store_file("sensitivity", "12\n\n", 4) == -EINVAL);
    CHECK("a sign of plus is taken", store_file("speed", "+7", 2) == 2 && tp_ram[0x60] == 7);
    CHECK("a NUL ends the text, and the count is the write's",
          store_file("speed", nul, sizeof(nul)) == (int32_t)sizeof(nul) && tp_ram[0x60] == 5);

    CHECK("1 to press_to_select", store_file("press_to_select", "1\n", 2) == 2);
    n_want = 0;
    expect(0xF5);
    expect_tp_toggle(0x2C, 0x01);
    expect(0xF4);
    CHECK("toggles its bit", guest_took_expected() && (tp_ram[0x2C] & 0x01) == 1);
    CHECK("y, already so, toggles nothing",
          store_file("press_to_select", "y", 1) == 1 && n_gtook == 2 && (tp_ram[0x2C] & 1) == 1);
    CHECK("\"of\" is false",
          store_file("press_to_select", "of", 2) == 2 && (tp_ram[0x2C] & 1) == 0);
    CHECK("\"on\" is true", store_file("press_to_select", "on", 2) == 2 && (tp_ram[0x2C] & 1) == 1);
    CHECK("N is false", store_file("press_to_select", "N", 1) == 1 && (tp_ram[0x2C] & 1) == 0);
    CHECK("e is no truth to Linux 6.12", store_file("press_to_select", "e", 1) == -EINVAL);
    CHECK("nor is o alone", store_file("press_to_select", "o", 1) == -EINVAL);
    CHECK("0 to ext_dev", store_file("ext_dev", "0", 1) == 1);
    n_want = 0;
    expect(0xF5);
    expect_tp_toggle(0x23, 0x02);
    expect(0xF4);
    CHECK("sets the bit that disables the device behind the stick",
          guest_took_expected() && (tp_ram[0x23] & 0x02) == 0x02);
    CHECK("and reads 0", show_file("ext_dev") == 2 && strcmp(shown, "0\n") == 0);
    CHECK("1 to skipback toggles location 0x2D's bit 3",
          store_file("skipback", "1", 1) == 1 && (tp_ram[0x2D] & 0x08) == 0x08);

    guest_forget();
    CHECK("with no stick a write fails", store_file("sensitivity", "100", 3) == -ENODEV);
    CHECK("and sends nothing", n_took == 0 && n_gtook == 0);
}

/* A write of the stick's sensitivity that waits for the command lock, and
 * what it returned. */
static int parked_attr;
static int32_t parked_result;

/** Enter the saved file's write, whose command wait parks it. */
static void parked_write(void)
{
    parked_result = attr_made[parked_attr].store(NULL, attr_made[parked_attr].arg, "100", 3);
}

/** A write of the stick's sensitivity parked behind a write of serio2's
 * `protocol` that switches the guest to PS/2: the switch takes the stick's
 * files away and keeps psmouse's, and the write fails with ENODEV and sends
 * the guest nothing, as Linux's `trackpoint_disconnect` removes the
 * stick's files under the switch. */
static void parked_rows(void)
{
    uint8_t ram, kept;
    int done, binds;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("the parked row brings a pad and its stick up", bring_up() == 0 && guest.active);
    n_attrs = 0;
    (void)trackpoint_sysfs_add(7);
    parked_attr = attr_named("sensitivity");
    if (parked_attr < 0) {
        CHECK("the stick has a sensitivity file", 0);
        return;
    }
    binds = driver_binds;
    kept = value[attr_made[parked_attr].arg];
    parked_result = 0;
    CHECK("a change of the guest takes the command lock",
          ps2mouse_guest_change_begin() == &synaptics_pt);
    done = park_run(parked_write);
    CHECK("the stick's write waits for it", done == 0 && cmd_busy);
    CHECK("bare switches the guest to PS/2",
          guest_set_protocol("bare", 4) == 0 && guest.protocol == PROTO_PS2 && guest.active);
    CHECK("which takes the stick's files away and keeps psmouse's",
          files_add == 0 && driver_add == guest_files && driver_binds == binds);
    ps2mouse_guest_change_end();
    ram = tp_ram[0x4A];
    n_took = 0;
    n_gtook = 0;
    if (!done) {
        CHECK("the waiting write returns", park_resume() == 1);
    }
    CHECK("with ENODEV", parked_result == -ENODEV);
    CHECK("sending the pad and the guest nothing", n_took == 0 && n_gtook == 0);
    CHECK("and changing no value",
          value[attr_made[parked_attr].arg] == kept && tp_ram[0x4A] == ram);
    CHECK("the pad and the guest report with the lock free",
          !cmd_busy && state == STATE_ACTIVE && pad_reporting && guest_reporting);
}

/** The reconnect after five bad packets in a row, as Linux's psmouse
 * asks `serio_reconnect` and `synaptics_reconnect` does it. */
static void reset_rows(void)
{
    static const uint8_t good[] = {0x80, 0x00, 0x00, 0xC0, 0x00, 0x00};
    int i;

    model_reset();
    CHECK("the Lenovo's pad is brought up", bring_up() == 0 && touchpad == 1);
    watch(&pad_w, node_of("isa0060/serio1/input0"));
    for (i = 0; i < 4; i++) {
        irq_byte(0x08);
    }
    CHECK("four bad packets in a row are counted",
          bad_in_a_row == 4 && state == STATE_ACTIVE && !reconnect_wanted);
    irq_byte(0x08);
    CHECK("the fifth stops the port and asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT &&
              starts_with(last_warning, "sync: %u bad packets in a row"));
    pad_packet(3000, 3000, 50, 8, 0, 0);
    take(&pad_w);
    CHECK("a packet after it is dropped", n_got == 0 && pktcnt == 0);
    n_took = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xFF);
    expect(0xF2);
    expect_knock();
    expect_queries_and_mode();
    expect(0xF4);
    CHECK("the reconnect is Linux's: reset, ID, knock, queries, mode, rate, reporting",
          pad_took_expected());
    CHECK("the port reports again", state == STATE_ACTIVE && bad_in_a_row == 0 && pad_reporting &&
                                        !reconnect_wanted && pad != NULL);
    pad_packet(3000, 3000, 50, 8, 0, 0);
    take(&pad_w);
    CHECK("and its packets are read", pktcnt == 0 && key_value(BTN_TOUCH) == 1);
    CHECK("every byte is read", model_count == 0);

    model_reset();
    CHECK("the pad is brought up again", bring_up() == 0);
    for (i = 0; i < 4; i++) {
        irq_byte(0x08);
    }
    irq_bytes(good, 6);
    CHECK("a whole packet ends the count",
          bad_in_a_row == 0 && starts_with(last_info, "sync: resynced"));
    for (i = 0; i < 4; i++) {
        irq_byte(0x08);
    }
    CHECK("so four more ask nothing",
          bad_in_a_row == 4 && state == STATE_ACTIVE && !reconnect_wanted);
}

/** A pad that answers the reconnect late, and another pad in its place. */
static void late_pad_rows(void)
{
    uint64_t before;
    int i, n;

    model_reset();
    CHECK("a pad that answers late is brought up", bring_up() == 0);
    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    knock_misses = 2;
    before = kernel_ticks;
    n_took = 0;
    ps2mouse_reconnect();
    n_want = 0;
    for (i = 0; i < 3; i++) {
        expect(0xFF);
        expect(0xF2);
        expect_knock();
    }
    expect_queries_and_mode();
    expect(0xF4);
    CHECK("it is asked three times", pad_took_expected());
    CHECK("a second apart", kernel_ticks - before == 2 * (uint64_t)MS_TO_TICKS(1000));
    CHECK("and reports again", state == STATE_ACTIVE && pad_reporting && touchpad == 1);

    model_reset();
    CHECK("a pad is brought up", bring_up() == 0);
    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    pad_model_low = 0xb2;
    n_took = 0;
    ps2mouse_reconnect();
    CHECK("another pad in its place is found by the reconnect",
          starts_with(last_warning, "reconnect: probing the port anew"));
    CHECK("and probed anew, as Linux's serio core rescans the port",
          n_took > 2 && took[0] == 0xFF && touchpad == 1 && info.model_id == 0x01e2b2 &&
              pad_reporting && state == STATE_ACTIVE);
    CHECK("with one event device", count_named("SynPS/2 Synaptics TouchPad") == 1);
    n = node_of("isa0060/serio1/input0");
    CHECK("at the PS/2 port", n >= 0);
}

/** The announcement 0xAA 0x00 of a device that reset itself. */
static void announce_rows(void)
{
    static const uint8_t good[] = {0x80, 0x00, 0x00, 0xC0, 0x00, 0x00};
    struct evdev *dev;
    unsigned int losses;

    model_reset();
    CHECK("the pad is brought up", bring_up() == 0);
    irq_byte(0xAA);
    CHECK("0xAA beginning a packet is held", bat_held && pktcnt == 0);
    irq_byte(0x00);
    CHECK("with 0x00 after it the port stops and asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT &&
              starts_with(last_warning, "the device announced itself anew"));
    n_took = 0;
    ps2mouse_reconnect();
    CHECK("the pad is reconnected",
          n_took > 0 && took[0] == 0xFF && state == STATE_ACTIVE && pad_reporting && !bat_held);

    losses = sync_losses;
    irq_byte(0xAA);
    irq_byte(0x80);
    CHECK("0xAA and another byte: the 0xAA is the packet's first byte, as Linux's relaxed "
          "check takes it",
          !bat_held && pktcnt == 2 && state == STATE_ACTIVE && !reconnect_wanted);
    irq_bytes(good + 2, 4);
    CHECK("and the packet is whole", pktcnt == 0 && sync_losses == losses);
    irq_bytes(good, 2);
    irq_byte(0xAA);
    CHECK("0xAA inside a packet is no announcement", !bat_held && pktcnt == 3);
    irq_bytes(good + 3, 3);

    model_reset();
    dev_plain = 1;
    CHECK("a plain mouse is brought up", bring_up() == 0 && touchpad == 0 && mouse_dev != NULL);
    irq_byte(0xAA);
    irq_byte(0x00);
    CHECK("its announcement asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT);
    n_took = 0;
    dev = mouse_dev;
    ps2mouse_reconnect();
    CHECK("which resets it and finds its protocol again",
          n_took > 2 && took[0] == 0xFF && took[1] == 0xF6 && pad_reporting &&
              state == STATE_ACTIVE && starts_with(last_info, "reconnect: the mouse reports"));
    CHECK("and it keeps its event device, as Linux's psmouse_reconnect keeps it",
          mouse_dev == dev && count_named("PS/2 Generic Mouse") == 1);
    irq_byte(0xAA);
    irq_byte(0x05);
    CHECK("a mouse's 0xAA with another byte is a packet's first byte",
          !bat_held && packet_index == 2);
    irq_byte(0x00);
    CHECK("and the packet is whole", packet_index == 0);
}

/** A plain mouse's bytes without the framing bit, counted as bad packets,
 * and its reconnect when its protocol is another after the reset. */
static void mouse_sync_rows(void)
{
    struct evdev *dev;
    int i;

    model_reset();
    dev_plain = 1;
    CHECK("a plain mouse is brought up", bring_up() == 0 && touchpad == 0 && mouse_dev != NULL);
    for (i = 0; i < 4; i++) {
        irq_byte(0x00);
    }
    CHECK("four first bytes without the framing bit are four bad packets",
          bad_in_a_row == 4 && state == STATE_ACTIVE && !reconnect_wanted && packet_index == 0);
    irq_bytes((const uint8_t[]){0x08, 0x01, 0x01}, 3);
    CHECK("a whole packet ends the count", bad_in_a_row == 0 && packet_index == 0);
    for (i = 0; i < 5; i++) {
        irq_byte(0x00);
    }
    CHECK("the fifth in a row stops the port and asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT &&
              starts_with(last_warning, "sync: %u bad packets in a row"));
    irq_byte(0x08);
    CHECK("the port then drops every byte", packet_index == 0);
    dev = mouse_dev;
    n_took = 0;
    ps2mouse_reconnect();
    CHECK("the mouse is reset and reports again with its event device",
          n_took > 2 && took[0] == 0xFF && mouse_dev == dev && pad_reporting &&
              state == STATE_ACTIVE && bad_in_a_row == 0);

    for (i = 0; i < 5; i++) {
        irq_byte(0x00);
    }
    dev_wheel = 1;
    ps2mouse_reconnect();
    CHECK("a mouse that answers the wheel's knock after its reset is probed anew",
          protocol == PROTO_IMPS && packet_size == 4 &&
              starts_with(last_warning, "reconnect: probing the port anew"));
    CHECK("its old device gone and one of the wheel's in its place",
          count_named("PS/2 Generic Mouse") == 0 &&
              count_named("ImPS/2 Generic Wheel Mouse") == 1 && pad_reporting &&
              state == STATE_ACTIVE);
}

/** The guest after its pad's reconnect, and after its own announcement. */
static void guest_reconnect_rows(void)
{
    int i;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("a pad with an IBM stick is brought up", bring_up() == 0);
    n_attrs = 0;
    (void)trackpoint_sysfs_add(7);
    CHECK("sensitivity is set to 200", store_file("sensitivity", "200", 3) == 3);
    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    n_gtook = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xE1);
    expect(0xE2);
    expect(0x7F);
    expect_tp_write(0x4A, 200); /* the one setting off its power-on value */
    expect_n(initialize_activate, 6);
    CHECK("the stick is reconnected as Linux's trackpoint_reconnect does", guest_took_expected());
    CHECK("it keeps its setting and reports",
          tp_ram[0x4A] == 200 && guest.active && guest_reporting && pad_reporting);
    CHECK("the pad's mode is the guest's", (pad_mode & MODE_FOUR_BYTE) == 0 && pad_mode == 0xC5);
    CHECK("every byte is read", model_count == 0);

    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    tp_por_fails = 1;
    n_gtook = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xE1);
    expect(0xE2);
    expect(0x7F);
    expect_ibm_sync(200);
    expect_n(initialize_activate, 6);
    CHECK("a stick whose reset fails then gets every setting", guest_took_expected());
}

/** The guest changed, gone or come at the pad's reconnect, and its own
 * announcement; after `guest_reconnect_rows`, whose stick it starts with. */
static void guest_change_rows(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    guest_kind = GUEST_MOUSE;
    ps2mouse_reconnect();
    CHECK("a mouse in the stick's place is probed anew",
          node_of("synaptics-pt/serio0/input0") >= 0 &&
              strcmp(desc.name, "PS/2 Generic Mouse") == 0);
    CHECK("and the stick's device is gone", count_named("TPPS/2 IBM TrackPoint") == 0);

    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    guest_kind = GUEST_NONE;
    n_took = 0;
    ps2mouse_reconnect();
    CHECK("a guest gone leaves no device",
          node_of("synaptics-pt/serio0/input0") < 0 && !guest.active);
    CHECK("and the pad reporting",
          pad_reporting && n_took > 0 && took[n_took - 1] == 0xF4 && state == STATE_ACTIVE);

    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    guest_kind = GUEST_TP;
    ps2mouse_reconnect();
    CHECK("a port that had none is probed anew, as Linux's serio core does",
          node_of("synaptics-pt/serio0/input0") >= 0 &&
              strcmp(desc.name, "TPPS/2 IBM TrackPoint") == 0 && guest.active);

    tp_por_fails = 0;
    guest_packet(0xAA, 0x00, 0x00, 0x00);
    CHECK("the stick's announcement asks for its reconnect alone",
          reconnect_wanted == RECONNECT_GUEST && !guest.active && state == STATE_ACTIVE &&
              starts_with(last_warning, "pass-through: the guest announced itself anew"));
    n_took = 0;
    n_gtook = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xE1);
    expect(0xE2);
    expect(0x7F);
    expect_n(initialize_activate, 6);
    CHECK("the stick is reconnected", guest_took_expected() && guest.active);
    CHECK("the pad is not reset, only disabled and enabled again",
          n_took > 0 && took[0] == 0xF5 && took[n_took - 1] == 0xF4 && pad_reporting &&
              touchpad == 1);
    CHECK("every byte is read", model_count == 0);
}

/** A write to a stick's file that begins in the middle of the pad's
 * packet: the bytes after it start a packet, as Linux's
 * `__psmouse_set_state` makes them. */
static void midpacket_rows(void)
{
    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    CHECK("a pad with an IBM stick is brought up", bring_up() == 0);
    n_attrs = 0;
    (void)trackpoint_sysfs_add(7);
    watch(&guest_w, node_of("synaptics-pt/serio0/input0"));
    watch(&pad_w, node_of("isa0060/serio1/input0"));
    pad_cut = 4;
    pad_packet(3000, 3000, 50, 8, 0, 0);
    pad_cut = 6;
    CHECK("four bytes of the pad's packet come", pktcnt == 4);
    CHECK("a write to sensitivity in the packet's middle",
          store_file("sensitivity", "100", 3) == 3 && tp_ram[0x4A] == 100);
    CHECK("drops the packet under way", pktcnt == 0 && bad_in_a_row == 0);
    pad_packet(3000, 3000, 50, 8, 0, 0);
    take(&guest_w);
    CHECK("the stick gets no packet made of the write's replies", n_got == 0);
    take(&pad_w);
    CHECK("and the pad's next packet is read whole", pktcnt == 0 && key_value(BTN_TOUCH) == 1);
    CHECK("with no packet counted bad", bad_in_a_row == 0 && model_count == 0);
}

/** A TrackPoint alone on the auxiliary port, as Linux's
 * `psmouse_extensions` finds one after the touchpads' probes. */
static void port_rows(void)
{
    static const struct event move[] = {
        {EV_KEY, BTN_LEFT, 1}, {EV_REL, REL_X, 5}, {EV_REL, REL_Y, 5}, {EV_SYN, SYN_REPORT, 0}};
    struct evdev *dev;
    int n, i;

    model_reset();
    dev_tp = 1;
    guest_kind = GUEST_TP;
    CHECK("a TrackPoint alone on the port is brought up", bring_up() == 0);
    CHECK("as a TrackPoint, no touchpad", touchpad == 0 && protocol == PROTO_TRACKPOINT);
    n = node_of("isa0060/serio1/input0");
    CHECK("its device is \"TPPS/2 IBM TrackPoint\" at isa0060/serio1/input0",
          n >= 0 && strcmp(desc.name, "TPPS/2 IBM TrackPoint") == 0);
    CHECK("bus i8042, vendor 2, product 10, Linux's PSMOUSE_TRACKPOINT",
          n >= 0 && desc.id[0] == EVDEV_BUS_I8042 && desc.id[1] == 0x0002 && desc.id[2] == 10);
    CHECK("a pointer and a pointing stick, with a middle button",
          n >= 0 && has_bit(desc.prop, INPUT_PROP_POINTER) &&
              has_bit(desc.prop, INPUT_PROP_POINTING_STICK) && has_bit(desc.key, BTN_MIDDLE));
    CHECK("its files are given to the port, serio1",
          files_port == 1 && files_add == trackpoint_sysfs_add);
    CHECK("it reports at Linux's rate", guest_reporting && n_gtook >= 6 &&
                                            gtook[n_gtook - 6] == 0xF3 &&
                                            gtook[n_gtook - 5] == 100);
    watch(&guest_w, n);
    irq_byte(0x29);
    irq_byte(0x05);
    irq_byte(0xFB);
    take(&guest_w);
    CHECK("its packet: left, 5 right and 5 toward the user", got_events(move, 4));

    n_attrs = 0;
    (void)trackpoint_sysfs_add(7);
    n_gtook = 0;
    CHECK("a write of 200 to sensitivity takes its count",
          store_file("sensitivity", "200", 3) == 3);
    n_want = 0;
    expect(0xF5);
    expect_tp_write(0x4A, 200);
    expect(0xF4);
    CHECK("the stick disabled, the value written, the stick enabled", guest_took_expected());
    CHECK("and the stick has it", tp_ram[0x4A] == 200 && guest_reporting);

    dev = mouse_dev;
    for (i = 0; i < 5; i++) {
        irq_byte(0x00);
    }
    CHECK("five bytes out of form ask for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT);
    n_gtook = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xE1);
    expect(0xE2);
    expect(0x7F);
    expect_tp_write(0x4A, 200);
    expect_n(initialize_activate, 6);
    CHECK("the reconnect is Linux's trackpoint_reconnect: ID, reset, the setting not at its "
          "power-on value, then the rate, resolution, scaling and reporting",
          guest_took_expected());
    CHECK("the stick keeps its device and its setting",
          mouse_dev == dev && tp_ram[0x4A] == 200 && state == STATE_ACTIVE && guest_reporting);
    CHECK("every byte is read", model_count == 0);
    dev_tp = 0;
}

/** A stick of a newer kind after its pad's reconnect. */
static void newer_kind_reconnect_rows(void)
{
    int i;

    model_reset();
    pad_caps |= PAD_PASS_THROUGH;
    guest_kind = GUEST_TP;
    tp_kind = TP_VARIANT_NXP;
    CHECK("a pad with an NXP stick is brought up", bring_up() == 0);
    for (i = 0; i < 5; i++) {
        irq_byte(0x08);
    }
    n_gtook = 0;
    ps2mouse_reconnect();
    n_want = 0;
    expect(0xE1);
    expect_tp_write(0x4A, 0x80);
    expect_tp_read(0x2C);
    expect_n(initialize_activate, 6);
    CHECK("a stick of a newer kind, never reset, gets its two settings again",
          guest_took_expected() && guest.active);
}

/** No table is mapped: the SMBIOS reader reads the row's from memory. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return 0;
}

/* The SMBIOS table a row gives, of System Information alone. */
static uint8_t smbios_table[256];

/** Give the SMBIOS reader a table whose System Information, type 1, names
 * the manufacturer `maker`, the product `product` and its `version`, or,
 * when `maker` is NULL, a table of no structure but its end. */
static void set_system(const char *maker, const char *product, const char *version)
{
    static const uint8_t end[6] = {127, 4, 0, 0, 0, 0};
    static const uint8_t header[8] = {1, 8, 0, 1, 1, 2, 3, 0};
    const char *const strings[3] = {maker, product, version};
    unsigned int n = 0, i, k;

    if (maker) {
        k_memcpy(smbios_table, header, sizeof(header));
        n = sizeof(header);
        for (k = 0; k < 3; k++) {
            for (i = 0; strings[k][i]; i++) {
                smbios_table[n++] = (uint8_t)strings[k][i];
            }
            smbios_table[n++] = 0;
        }
        smbios_table[n++] = 0;
    }
    k_memcpy(smbios_table + n, end, sizeof(end));
    smbios_set_table(smbios_table, n + (unsigned int)sizeof(end));
}

int main(void)
{
    set_system(NULL, NULL, NULL);
    pnp_rows();
    min_max_rows();
    topbutton_rows();
    forcepad_rows();
    smbus_rows();
    kind_rows();
    ibm_rows();
    framing_rows();
    mouse_rows();
    packet_rows();
    file_rows();
    parked_rows();
    reset_rows();
    late_pad_rows();
    announce_rows();
    mouse_sync_rows();
    guest_reconnect_rows();
    guest_change_rows();
    newer_kind_reconnect_rows();
    midpacket_rows();
    port_rows();
    model_reset();
    if (fails) {
        printf("trackpoint-test: %d failures\n", fails);
        return fails > 255 ? 255 : fails;
    }
    printf("trackpoint-test: ok\n");
    return 0;
}
