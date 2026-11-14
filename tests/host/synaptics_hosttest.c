/**
 * The Synaptics touchpad's detection, setup and packets (sys/arch/x86/
 * synaptics.c), through the PS/2 mouse's bring-up and IRQ 12 handler
 * (ps2mouse.c), against a model of the 8042 and of the Lenovo's touchpad
 * behind it, with the event device read as a program reads it (evdev.c)
 * and the mouse device's packets (mouse.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The driver's files are included whole, so that their static state is
 * reachable. The model touchpad is the Lenovo's as Linux 6.x's psmouse
 * logged it (dev-log/70-wifi-chip/linux/dmesg.txt): model 1, firmware 8.1,
 * model ID 0x1e2b1, capabilities 0xd00123, 0x840300, 0x2a800 and 0, board
 * 2848, firmware ID 1581875, coordinates from 1314 to 5670 and from 1098 to
 * 4794. That log does not give the resolution, which the model answers as
 * 42 and 59 units a millimetre. The model takes a sliced request as the
 * firmware does: four resolutions after any other command make a byte, two
 * bits each from the top, which a status request (0xE9) answers as a query
 * and a sample rate of 0x14 sets as the mode byte, 0xC8 turning on the
 * advanced gesture mode. Without the four resolutions a status request
 * reads a mouse's status, resolution and rate. A row can make the model a
 * plain mouse, have it leave one query unanswered, or leave every byte
 * 0x14, the mode's rate, unacknowledged.
 *
 * The capabilities the event device must have are what Linux's
 * `set_input_params` and `input_mt_init_slots` give this pad, as Debian's
 * kernel lists them in /proc/bus/input/devices: the test prints the bits
 * EVIOCGBIT gives in that file's form and compares the strings. The probe
 * for an ALPS touchpad, which alps_hosttest.c tests, finds none here.
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

static int fails;

#define CHECK(what, cond)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("synaptics-test: fail: %s\n", what); \
            fails++;                                    \
        }                                               \
    } while (0)

/* The status of a byte the mouse sent, and the configuration byte the
 * firmware leaves and the one a bring-up writes. */
#define MOUSE_BYTE      (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)
#define FIRMWARE_CONFIG 0x65
#define MOUSE_CONFIG    0x47

/* The ticks of the kernel's clock, which a row moves on. */
volatile uint64_t kernel_ticks;

/* The model 8042's output buffer, a queue of bytes the touchpad sent. */
#define MODEL_BYTES 32
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;

/* The controller's configuration byte, and what the next write to port
 * 0x60 is for: 0, or the command that named it. */
static uint8_t ctl_config;
static uint8_t ctl_next;

/* What the auxiliary device is. */
#define KIND_PAD   1 /* the Lenovo's Synaptics touchpad */
#define KIND_PLAIN 2 /* a PS/2 mouse of three buttons and no wheel */

/* The device: its kind; a query whose status request it leaves without
 * data, -1 for none; whether it leaves the mode's 0x14 unacknowledged; the
 * command whose argument comes next, 0 for none; the byte the resolutions
 * since the last other command make, and their count; its mode byte,
 * resolution, rate and whether it reports; and every byte it took. */
static int dev_kind;
static int silent_query;
static int refuse_mode;
static uint8_t arg_for;
static uint8_t special;
static int res_count;
static uint8_t pad_mode;
static int modes_set;      /* the mode bytes set since the model's reset */
static uint8_t first_mode; /* the first of them */
static int agm_on;
static uint8_t dev_res;
static uint8_t dev_rate;
static int reporting;
#define TOOK_MAX 512
static uint8_t took[TOOK_MAX];
static int n_took;

/* Interrupts as `arch_irq_disable` and `arch_irq_enable` leave them, the
 * port accesses made while they were on, the handler registered, and the
 * warnings logged. */
static int irq_on = 1;
static int ports_with_irq_on;
static isr_handler_t registered_handler;
static int warnings;
static const char *last_warning;
static const char *last_info;

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("synaptics-test: fail: the model's output buffer overflowed\n");
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

/** Answer the query `q` as the Lenovo's touchpad answers it. */
static void answer_query(uint8_t q)
{
    if (q == silent_query) {
        return;
    }
    switch (q) {
    case 0x00: /* identify: minor 1, 0x47, model 1 and major 8 */
        put3(0x01, 0x47, 0x18);
        break;
    case 0x01: /* modes: board 2848 and no query 0x10, then the mode */
        put3(0x2c, 0x20, pad_mode);
        break;
    case 0x02:
        put3(0xd0, 0x01, 0x23);
        break;
    case 0x03:
        put3(0x01, 0xe2, 0xb1);
        break;
    case 0x08: /* 42 and 59 units a millimetre */
        put3(42, 0x80, 59);
        break;
    case 0x09:
        put3(0x84, 0x03, 0x00);
        break;
    case 0x0a:
        put3(0x18, 0x23, 0x33);
        break;
    case 0x0c:
        put3(0x02, 0xa8, 0x00);
        break;
    case 0x0d: /* 5670 and 4794 */
        put3(0xb1, 0xd3, 0x95);
        break;
    case 0x0f: /* 1314 and 1098 */
        put3(0x29, 0x51, 0x22);
        break;
    default:
        put3(0, 0, 0);
        break;
    }
}

/** Take `b`, a byte the controller sent on to the device. */
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
    if (refuse_mode && b == 0x14) {
        arg_for = 0;
        res_count = 0;
        return; /* the mode's rate, and every try of it again, unanswered */
    }
    if (arg_for) {
        cmd = arg_for;
        arg_for = 0;
        if (cmd == 0xE8) {
            model_put(0xFA);
            special = (uint8_t)(special << 2 | (b & 3u));
            res_count++;
            dev_res = b;
            return;
        }
        model_put(0xFA);
        if (dev_kind == KIND_PAD && res_count >= 4 && b == 0x14) {
            pad_mode = special;
            if (modes_set++ == 0) {
                first_mode = special;
            }
        } else if (dev_kind == KIND_PAD && res_count >= 4 && b == 0xC8) {
            agm_on = 1;
        } else {
            dev_rate = b;
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
        if (dev_kind == KIND_PAD && res_count >= 4) {
            answer_query(special);
        } else {
            put3(reporting ? 0x20 : 0x00, dev_res, dev_rate);
        }
        break;
    case 0xF2:
        model_put(0x00);
        break;
    case 0xFF:
        model_put(0xAA);
        model_put(0x00);
        pad_mode = 0;
        agm_on = 0;
        reporting = 0;
        dev_res = 2;
        dev_rate = 100;
        break;
    case 0xF4:
        reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        reporting = 0;
        break;
    default:
        break;
    }
    res_count = 0;
}

uint8_t inb(uint16_t port)
{
    uint8_t data;

    if (irq_on) {
        ports_with_irq_on++;
    }
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

    if (irq_on) {
        ports_with_irq_on++;
    }
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
{
    irq_on = 0;
}

void arch_irq_enable(void)
{
    irq_on = 1;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)name;
    registered_handler = handler;
}

/** No Elantech touchpad answers when no Synaptics pad does: its probe is
 * elantech_hosttest.c's. */
int elantech_init(void)
{
    return 0;
}

/** No Elantech touchpad's bytes come, since `elantech_init` found none. */
int elantech_byte(uint8_t data)
{
    (void)data;
    return ELANTECH_GOOD;
}

/* The rest of elantech.h, which a port without an Elantech pad never
 * calls; elantech_hosttest.c tests it. */
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

/** No ALPS touchpad answers. */
int alps_init(void)
{
    return 0;
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

/** No ALPS pad's set-up fails, so no mouse takes its name. */
const char *alps_mouse_name(const char *proto)
{
    return proto;
}

/** The console's blanking is not modelled. */
void console_poke(void)
{}

/* What evdev.c needs: one processor, fixed clocks, nothing to repeat,
 * reads that never sleep, since every open is nonblocking, and calls of
 * x86-64 programs. */

/** Every call here is an x86-64 program's. */
int in_compat_syscall(void)
{
    return 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

/* What the pass-through port's guest needs, which this pad, without the
 * port, never calls; trackpoint_hosttest.c tests the guest. */
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

/** This model has no competing command caller. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    (void)port;
    (void)add;
}

void evdev_sysfs_serio_driver(int port, int (*add)(int dir))
{
    (void)port;
    (void)add;
}

int evdev_sysfs_serio_manual(int port)
{
    (void)port;
    return 0;
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

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
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

/** No row writes to an event device. */
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

/* The ioctls, by Linux's macros: _IOC(dir, 'E', nr, size). */
#define IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('E' << 8) | (nr))
#define EVIOCGID           0x80084502u
#define EVIOCGNAME(n)      IOC(2, 0x06, n)
#define EVIOCGPHYS(n)      IOC(2, 0x07, n)
#define EVIOCGPROP(n)      IOC(2, 0x09, n)
#define EVIOCGMTSLOTS(n)   IOC(2, 0x0a, n)
#define EVIOCGBIT(t, n)    IOC(2, 0x20 + (t), n)
#define EVIOCGABS(a)       IOC(2, 0x40 + (a), sizeof(struct input_absinfo))

/* An evdev record as the host's C library lays it out, 24 bytes. */
struct record {
    unsigned long sec;
    unsigned long usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* An open of the event device: its descriptor and description. */
static fd_entry_t open_e;
static struct open_file open_ofd;
static int is_open;

/* The records the last `take` read. */
static struct record got[64];
static int n_got;

/** Open the event device that `init_mouse` registered, node 0, the only
 * one, nonblocking. Returns what `evdev_open` returns. */
static int32_t open_pad(void)
{
    int32_t rc;

    k_memset(&open_e, 0, sizeof(open_e));
    k_memset(&open_ofd, 0, sizeof(open_ofd));
    open_ofd.status_flags = OFD_NONBLOCK;
    open_e.ofd = &open_ofd;
    rc = evdev_open(&open_e, 0);
    is_open = rc == 0;
    return rc;
}

/** Read every whole packet into `got`. */
static int32_t take(void)
{
    int32_t rc = evdev_read(&open_e, (char __user *)got, sizeof(got));

    n_got = rc > 0 ? rc / (int32_t)sizeof(got[0]) : 0;
    return rc;
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
        printf("synaptics-test: read %d records, not %d\n", n_got, n);
        for (i = 0; i < n_got; i++) {
            printf("synaptics-test:   %u %u %d\n", got[i].type, got[i].code, (int)got[i].value);
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (got[i].type != want[i].type || got[i].code != want[i].code ||
            got[i].value != want[i].value) {
            printf("synaptics-test: record %d is %u %u %d, not %u %u %d\n", i, got[i].type,
                   got[i].code, (int)got[i].value, want[i].type, want[i].code, (int)want[i].value);
            return 0;
        }
    }
    return 1;
}

/** Put the controller and the device as the firmware leaves them, with a
 * device of `kind` and no fault, and take away what the last row
 * registered. */
static void model_reset(int kind)
{
    if (is_open) {
        evdev_close(&open_e);
        is_open = 0;
    }
    evdev_unregister(pad);
    pad = NULL;
    evdev_unregister(mouse_dev);
    mouse_dev = NULL;
    touchpad = 0;
    model_head = 0;
    model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_kind = kind;
    silent_query = -1;
    refuse_mode = 0;
    arg_for = 0;
    special = 0;
    res_count = 0;
    pad_mode = 0;
    modes_set = 0;
    first_mode = 0;
    agm_on = 0;
    dev_res = 2;
    dev_rate = 100;
    reporting = 0;
    n_took = 0;
    irq_on = 1;
    ports_with_irq_on = 0;
    registered_handler = 0;
    warnings = 0;
    last_warning = 0;
    last_info = 0;
    pktcnt = 0;
    sync_losses = 0;
    k_memset(&agm, 0, sizeof(agm));
    agm_count = 0;
}

/* The device's bytes a row expects, built with the `expect_*` calls. */
static uint8_t want_took[TOOK_MAX];
static int n_want;

/** Expect the byte `b` sent to the device. */
static void expect(uint8_t b)
{
    if (n_want < TOOK_MAX) {
        want_took[n_want] = b;
    }
    n_want++;
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

/** Expect the bring-up's start, the reset and the defaults, a Kensington
 * ThinkingMouse's rates and ID, which psmouse tries first, and the
 * Synaptics knock, Linux's `synaptics_detect`. */
static void expect_start(void)
{
    static const uint8_t thinking[9] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
    int i;

    n_want = 0;
    expect(0xFF);
    expect(0xF6);
    expect(0xF3);
    expect(10);
    expect(0xE8);
    expect(0x00);
    for (i = 0; i < 9; i++) {
        expect(0xF3);
        expect(thinking[i]);
    }
    expect(0xF2);
    for (i = 0; i < 4; i++) {
        expect(0xE8);
        expect(0x00);
    }
    expect(0xE9);
}

/** Whether the device took the bytes expected, the first `n` of them when
 * `n` is not 0; prints the first that differs. */
static int took_expected(int n)
{
    int i, count = n ? n : n_want;

    if (!n && n_took != n_want) {
        printf("synaptics-test: the device took %d bytes, not %d\n", n_took, n_want);
    }
    for (i = 0; i < count && i < n_took && i < TOOK_MAX; i++) {
        if (took[i] != want_took[i]) {
            printf("synaptics-test: byte %d to the device is 0x%02x, not 0x%02x\n", i, took[i],
                   want_took[i]);
            return 0;
        }
    }
    return n ? n_took >= n : n_took == n_want;
}

/** The Lenovo's touchpad brought up: the commands of Linux's psmouse in
 * their order, the mode it is left in, and the handler. */
static void bringup_rows(void)
{
    static const uint8_t queries[] = {0x00, 0x03, 0x0a, 0x01, 0x02, 0x09, 0x0c, 0x08, 0x0d, 0x0f};
    unsigned i;

    model_reset(KIND_PAD);
    CHECK("the touchpad's bring-up succeeds", init_mouse() == 0);
    CHECK("the touchpad speaks the absolute protocol", touchpad == 1);
    expect_start();
    expect(0xFF); /* psmouse_reset */
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
    expect(0xF4);
    CHECK("the commands are Linux's, in Linux's order", took_expected(0));
    CHECK("absolute mode, 80 packets a second, gestures off, W mode", pad_mode == 0xC5);
    CHECK("the advanced gesture mode is on", agm_on);
    CHECK("the touchpad reports", reporting);
    CHECK("the configuration byte enables IRQ 12 and the aux clock", ctl_config == MOUSE_CONFIG);
    CHECK("the queries are read", info.identity == 0x014718 && info.model_id == 0x01e2b1 &&
                                      info.firmware_id == 1581875 && info.board_id == 2848 &&
                                      info.capabilities == 0xd00123 && info.ext_cap == 0x840300 &&
                                      info.ext_cap_0c == 0x02a800 && info.ext_cap_10 == 0);
    CHECK("the limits are queried, the minimum as firmware 8.1 allows",
          info.x_min == 1314 && info.x_max == 5670 && info.y_min == 1098 && info.y_max == 4794 &&
              info.x_res == 42 && info.y_res == 59);
    CHECK("its packets are of the new absolute form", pkt_type == PKT_NEWABS);
    CHECK("the handler is registered", registered_handler == mouse_irq_handler);
    CHECK("the bring-up touches no port with interrupts on", ports_with_irq_on == 0);
    CHECK("the bring-up turns interrupts on again", irq_on);
    CHECK("the bring-up logs the touchpad and no warning", warnings == 0 && last_info != 0);
    CHECK("no mouse device is registered beside it", mouse_dev == NULL);
    CHECK("every byte is read", model_count == 0);
}

/** `n` longs of the bits at `bytes`, as /proc/bus/input/devices prints a
 * set: from the highest long that is not 0, each in hex, the lower ones 0
 * too, or a single 0. */
static void proc_bits(char *out, const uint8_t *bytes, int n)
{
    int i, j, started = 0;
    unsigned long long word;

    out[0] = '\0';
    for (i = n - 1; i >= 0; i--) {
        word = 0;
        for (j = 7; j >= 0; j--) {
            word = word << 8 | bytes[i * 8 + j];
        }
        if (!word && !started) {
            continue;
        }
        out += sprintf(out, "%s%llx", started ? " " : "", word);
        started = 1;
    }
    if (!started) {
        sprintf(out, "0");
    }
}

/** Whether EVIOCGABS of `code` gives `min`, `max`, `fuzz` and `res`. */
static int abs_is(uint16_t code, int32_t min, int32_t max, int32_t fuzz, int32_t res)
{
    struct input_absinfo a;

    if (evdev_ioctl(&open_e, EVIOCGABS(code), (uintptr_t)&a) != 0) {
        return 0;
    }
    return a.minimum == min && a.maximum == max && a.fuzz == fuzz && a.flat == 0 &&
           a.resolution == res;
}

/** The event device's name, numbers and capabilities, against what Debian's
 * kernel gives the same pad. */
static void capability_rows(void)
{
    uint8_t bits[96];
    uint16_t id[4];
    int32_t slots[3];
    char name[64];
    char text[256];

    CHECK("the touchpad's event device opens", open_pad() == 0);
    CHECK("its name is SynPS/2 Synaptics TouchPad",
          evdev_ioctl(&open_e, EVIOCGNAME(64), (uintptr_t)name) > 0 &&
              strcmp(name, "SynPS/2 Synaptics TouchPad") == 0);
    CHECK("its place is isa0060/serio1/input0",
          evdev_ioctl(&open_e, EVIOCGPHYS(64), (uintptr_t)name) > 0 &&
              strcmp(name, "isa0060/serio1/input0") == 0);
    evdev_ioctl(&open_e, EVIOCGID, (uintptr_t)id);
    CHECK("I: Bus=0011 Vendor=0002 Product=0007 Version=01b1",
          id[0] == 0x11 && id[1] == 2 && id[2] == 7 && id[3] == 0x01b1);

    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e, EVIOCGPROP(8), (uintptr_t)bits);
    proc_bits(text, bits, 1);
    CHECK("B: PROP=1, a pointer and no buttonpad", strcmp(text, "1") == 0);
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e, EVIOCGBIT(0, 8), (uintptr_t)bits);
    proc_bits(text, bits, 1);
    CHECK("B: EV=b", strcmp(text, "b") == 0);
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e, EVIOCGBIT(EV_KEY, 96), (uintptr_t)bits);
    proc_bits(text, bits, 12);
    if (strcmp(text, "e520 30000 0 0 0 0") != 0) {
        printf("synaptics-test: KEY=%s\n", text);
    }
    CHECK("B: KEY=e520 30000 0 0 0 0", strcmp(text, "e520 30000 0 0 0 0") == 0);
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e, EVIOCGBIT(EV_ABS, 8), (uintptr_t)bits);
    proc_bits(text, bits, 1);
    if (strcmp(text, "660800011000003") != 0) {
        printf("synaptics-test: ABS=%s\n", text);
    }
    CHECK("B: ABS=660800011000003", strcmp(text, "660800011000003") == 0);
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e, EVIOCGBIT(EV_REL, 8), (uintptr_t)bits);
    proc_bits(text, bits, 1);
    CHECK("no relative axis", strcmp(text, "0") == 0);

    CHECK("ABS_X from 1314 to 5670, 42 a millimetre", abs_is(ABS_X, 1314, 5670, 0, 42));
    CHECK("ABS_Y from 1098 to 4794, 59 a millimetre", abs_is(ABS_Y, 1098, 4794, 0, 59));
    CHECK("ABS_PRESSURE to 255", abs_is(ABS_PRESSURE, 0, 255, 0, 0));
    CHECK("ABS_TOOL_WIDTH to 15", abs_is(ABS_TOOL_WIDTH, 0, 15, 0, 0));
    CHECK("two slots", abs_is(ABS_MT_SLOT, 0, 1, 0, 0));
    CHECK("ABS_MT_POSITION_X as ABS_X", abs_is(ABS_MT_POSITION_X, 1314, 5670, 0, 42));
    CHECK("ABS_MT_POSITION_Y as ABS_Y", abs_is(ABS_MT_POSITION_Y, 1098, 4794, 0, 59));
    CHECK("ABS_MT_TRACKING_ID to 65535", abs_is(ABS_MT_TRACKING_ID, 0, 65535, 0, 0));
    CHECK("ABS_MT_PRESSURE to 255", abs_is(ABS_MT_PRESSURE, 0, 255, 0, 0));

    slots[0] = ABS_MT_TRACKING_ID;
    slots[1] = slots[2] = 7;
    CHECK("EVIOCGMTSLOTS gives each slot's tracking number",
          evdev_ioctl(&open_e, EVIOCGMTSLOTS(sizeof(slots)), (uintptr_t)slots) == 0 &&
              slots[1] == -1 && slots[2] == -1);
    slots[0] = ABS_X;
    CHECK("EVIOCGMTSLOTS of an axis that is no slot's is -EINVAL",
          evdev_ioctl(&open_e, EVIOCGMTSLOTS(sizeof(slots)), (uintptr_t)slots) == -EINVAL);
}

/** Run IRQ 12 once with the touchpad's byte `b` waiting. */
static void irq_byte(uint8_t b)
{
    model_head = 0;
    model_count = 0;
    model_put(b);
    mouse_irq_handler(0);
}

/** Send a packet of the new absolute form: one finger, or two (W 0), with
 * the pad's Y, which grows upward, and the buttons `left` and `right`. */
static void send_packet(int x, int y, int z, int w, int left, int right)
{
    int buttons = left | right << 1;

    irq_byte((uint8_t)(0x80 | (w & 0x0c) << 2 | (w & 0x02) << 1 | buttons));
    irq_byte((uint8_t)(((y >> 8) & 0x0f) << 4 | ((x >> 8) & 0x0f)));
    irq_byte((uint8_t)z);
    irq_byte(
        (uint8_t)(0xC0 | ((y >> 12) & 1) << 5 | ((x >> 12) & 1) << 4 | (w & 1) << 2 | buttons));
    irq_byte((uint8_t)(x & 0xff));
    irq_byte((uint8_t)(y & 0xff));
}

/** Send an advanced gesture packet of the second finger at `x`, `y`,
 * pressure `z`, at the half resolution it is sent in. */
static void send_agm(int x, int y, int z)
{
    int hx = x >> 1, hy = y >> 1, hz = z >> 1;

    irq_byte(0x84); /* W 2 */
    irq_byte((uint8_t)(hx & 0xff));
    irq_byte((uint8_t)(hy & 0xff));
    irq_byte((uint8_t)(0xC0 | (hz & 0x30)));
    irq_byte((uint8_t)(((hy >> 8) & 0x0f) << 4 | ((hx >> 8) & 0x0f)));
    irq_byte((uint8_t)(0x10 | (hz & 0x0f))); /* packet type 1 */
}

/** One finger, a second, both lifted, the buttons, and packets out of
 * form, read from the event device as Linux's evdev gives them. */
static void packet_rows(void)
{
    static const struct event one[] = {{EV_ABS, ABS_MT_TRACKING_ID, 0},
                                       {EV_ABS, ABS_MT_POSITION_X, 3000},
                                       {EV_ABS, ABS_MT_POSITION_Y, 3356},
                                       {EV_ABS, ABS_MT_PRESSURE, 50},
                                       {EV_KEY, BTN_TOUCH, 1},
                                       {EV_ABS, ABS_X, 3000},
                                       {EV_ABS, ABS_Y, 3356},
                                       {EV_ABS, ABS_PRESSURE, 50},
                                       {EV_KEY, BTN_TOOL_FINGER, 1},
                                       {EV_SYN, SYN_REPORT, 0}};
    static const struct event two[] = {
        {EV_ABS, ABS_MT_POSITION_X, 3010}, {EV_ABS, ABS_MT_PRESSURE, 60},
        {EV_ABS, ABS_MT_SLOT, 1},          {EV_ABS, ABS_MT_TRACKING_ID, 1},
        {EV_ABS, ABS_MT_POSITION_X, 4000}, {EV_ABS, ABS_MT_POSITION_Y, 4056},
        {EV_ABS, ABS_MT_PRESSURE, 40},     {EV_ABS, ABS_X, 3010},
        {EV_ABS, ABS_PRESSURE, 60},        {EV_KEY, BTN_TOOL_FINGER, 0},
        {EV_KEY, BTN_TOOL_DOUBLETAP, 1},   {EV_SYN, SYN_REPORT, 0}};
    static const struct event lift[] = {
        {EV_ABS, ABS_MT_SLOT, 0},        {EV_ABS, ABS_MT_TRACKING_ID, -1},
        {EV_ABS, ABS_MT_SLOT, 1},        {EV_ABS, ABS_MT_TRACKING_ID, -1},
        {EV_KEY, BTN_TOUCH, 0},          {EV_ABS, ABS_PRESSURE, 0},
        {EV_KEY, BTN_TOOL_DOUBLETAP, 0}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event press[] = {{EV_KEY, BTN_LEFT, 1}, {EV_SYN, SYN_REPORT, 0}};
    static const struct event again[] = {{EV_ABS, ABS_MT_SLOT, 0},
                                         {EV_ABS, ABS_MT_TRACKING_ID, 2},
                                         {EV_ABS, ABS_MT_POSITION_X, 2000},
                                         {EV_ABS, ABS_MT_POSITION_Y, 3856},
                                         {EV_ABS, ABS_MT_PRESSURE, 40},
                                         {EV_KEY, BTN_TOUCH, 1},
                                         {EV_ABS, ABS_X, 2000},
                                         {EV_ABS, ABS_Y, 3856},
                                         {EV_ABS, ABS_PRESSURE, 40},
                                         {EV_KEY, BTN_TOOL_FINGER, 1},
                                         {EV_KEY, BTN_LEFT, 0},
                                         {EV_SYN, SYN_REPORT, 0}};
    int32_t slots[3];

    take();
    send_packet(3000, 2500, 50, 8, 0, 0);
    CHECK("a packet's bytes are read whole", pktcnt == 0 && sync_losses == 0);
    CHECK("the packets are of the strict form", pkt_type == PKT_NEWABS_STRICT);
    CHECK("one finger is a contact in slot 0 and the pointer at its place, Y turned over",
          take() > 0 && got_events(one, 10));

    send_agm(4000, 1800, 40);
    CHECK("an advanced gesture packet is kept for the next", take() == -EAGAIN);
    send_packet(3010, 2500, 60, 0, 0, 0);
    CHECK("the second finger takes slot 1 and the first keeps slot 0",
          take() > 0 && got_events(two, 12));
    slots[0] = ABS_MT_POSITION_X;
    CHECK("EVIOCGMTSLOTS gives both contacts' X",
          evdev_ioctl(&open_e, EVIOCGMTSLOTS(sizeof(slots)), (uintptr_t)slots) == 0 &&
              slots[1] == 3010 && slots[2] == 4000);

    send_packet(0, 0, 0, 0, 0, 0);
    CHECK("no pressure lifts both slots and the touch", take() > 0 && got_events(lift, 8));

    send_packet(0, 0, 0, 0, 1, 0);
    CHECK("the left button", take() > 0 && got_events(press, 2));

    /* A first byte of the relative form, then a whole packet. */
    irq_byte(0x08);
    CHECK("a byte out of form is dropped", pktcnt == 0 && sync_losses == 1);
    irq_byte(0x80);
    irq_byte(0x00);
    irq_byte(0x14);
    kernel_ticks += MS_TO_TICKS(600);
    irq_byte(0);
    CHECK("a pause inside the pad's packet queues a resync",
          state == STATE_RESYNC && (reconnect_wanted & RESYNC_PORT));
    ps2mouse_resync();
    CHECK("the unanswered poll queues a reconnect",
          state == STATE_IGNORE && (reconnect_wanted & RECONNECT_PORT));
    ps2mouse_reconnect();
    CHECK("the pad reports after reconnect", state == STATE_ACTIVE);

    send_packet(2000, 2000, 40, 8, 0, 0);
    CHECK("the driver does not count the pause as a malformed packet", sync_losses == 1);
    CHECK("the packet after it is read, a new contact in slot 0",
          take() > 0 && got_events(again, 12));
}

/** Read the mouse device's packets into `buf`; returns their count. */
static int mouse_packets(struct mouse_packet *buf, int max)
{
    return mouse_read_packets(buf, max);
}

/** The mouse device's packets from the touchpad, as Linux's mousedev makes
 * them: the movement from the third packet of a touch, and a tap. */
static void mouse_rows(void)
{
    struct mouse_packet p[16];
    int n;

    send_packet(0, 0, 0, 0, 0, 0);
    mouse_clear_packets();
    send_packet(3000, 2000, 50, 8, 0, 0);
    send_packet(3100, 2000, 50, 8, 0, 0);
    send_packet(3200, 2000, 50, 8, 0, 0);
    send_packet(3300, 2000, 50, 8, 0, 0);
    n = mouse_packets(p, 16);
    CHECK("each packet of a touch gives a mouse packet", n == 4);
    CHECK("the first two do not move", n == 4 && p[0].dx == 0 && p[1].dx == 0);
    /* 200 units over the pad's 4356 are 11.75 counts: 11, then 12 with the
     * fraction carried. */
    CHECK("then each moves by the change over two packets, a pad's width 256 counts",
          n == 4 && p[2].dx == 11 && p[3].dx == 12 && p[2].dy == 0 && p[3].dy == 0);
    kernel_ticks += MS_TO_TICKS(300);
    send_packet(0, 0, 0, 0, 0, 0);
    n = mouse_packets(p, 16);
    CHECK("a long touch's lift is no click", n == 1 && p[0].buttons == 0);

    send_packet(3000, 3000, 50, 8, 0, 0);
    kernel_ticks += MS_TO_TICKS(100);
    send_packet(0, 0, 0, 0, 0, 0);
    n = mouse_packets(p, 16);
    CHECK("a short touch is a click of the left button",
          n == 3 && p[0].buttons == 0 && p[1].buttons == 1 && p[1].dx == 0 && p[2].buttons == 0);
    send_packet(0, 0, 0, 0, 0, 1);
    n = mouse_packets(p, 16);
    CHECK("the right button", n == 1 && p[0].buttons == 2);
    send_packet(0, 0, 0, 0, 0, 0);
    mouse_clear_packets();
}

/** The events a program writes to a mouse's event device, as mousedev takes
 * them: a packet at each SYN_REPORT after a change, its motion up positive. */
static void written_rows(void)
{
    struct mouse_packet p[4];
    int n;

    mouse_clear_packets();
    mouse_written(EV_REL, REL_X, 5);
    mouse_written(EV_REL, REL_Y, 3);
    mouse_written(EV_SYN, SYN_REPORT, 0);
    n = mouse_packets(p, 4);
    CHECK("a written motion is a packet", n == 1 && p[0].dx == 5 && p[0].dy == -3);
    mouse_written(EV_KEY, BTN_LEFT, 1);
    mouse_written(EV_SYN, SYN_REPORT, 0);
    mouse_written(EV_SYN, SYN_REPORT, 0);
    n = mouse_packets(p, 4);
    CHECK("a written press is one packet", n == 1 && p[0].buttons == 1 && p[0].dx == 0);
    mouse_written(EV_KEY, BTN_LEFT, 0);
    mouse_written(EV_SYN, SYN_REPORT, 0);
    n = mouse_packets(p, 4);
    CHECK("and its release another", n == 1 && p[0].buttons == 0);
    mouse_clear_packets();
}

/** Whether the last warning says the touchpad did not answer a query. */
static int warned_query(void)
{
    return last_warning != 0 && strcmp(last_warning, "query: the touchpad did not answer") == 0;
}

/** A plain mouse, and a Synaptics pad whose setup fails, go on as mice. */
static void fallback_rows(void)
{
    char name[64];

    model_reset(KIND_PLAIN);
    CHECK("a plain mouse's bring-up succeeds", init_mouse() == 0);
    CHECK("a plain mouse is no touchpad", touchpad == 0 && pad == NULL);
    expect_start();
    expect(0xE8); /* a Genius mouse's resolution, scalings and status */
    expect(0x03);
    expect(0xE6);
    expect(0xE6);
    expect(0xE6);
    expect(0xE9);
    expect(0xE1); /* the TrackPoint's read-ID, then the IntelliMouse's knock */
    expect(0xF3);
    CHECK("a plain mouse is knocked as Linux's psmouse knocks", took_expected(n_want));
    CHECK("it is PS/2 Generic Mouse",
          open_pad() == 0 && evdev_ioctl(&open_e, EVIOCGNAME(64), (uintptr_t)name) > 0 &&
              strcmp(name, "PS/2 Generic Mouse") == 0);
    CHECK("a plain mouse's bring-up warns of nothing", warnings == 0);

    model_reset(KIND_PAD);
    silent_query = 0x03;
    CHECK("a touchpad that leaves a query unanswered is brought up", init_mouse() == 0);
    CHECK("as a mouse whose setup failed", touchpad == -1 && pad == NULL);
    CHECK("its failure is logged", warned_query());
    CHECK("it is set back to relative mode", pad_mode == 0 && reporting);
    CHECK("the failed setup touches no port with interrupts on", ports_with_irq_on == 0);
    CHECK("it is PS/2 Synaptics TouchPad",
          open_pad() == 0 && evdev_ioctl(&open_e, EVIOCGNAME(64), (uintptr_t)name) > 0 &&
              strcmp(name, "PS/2 Synaptics TouchPad") == 0);
    take();
    irq_byte(0x09);
    irq_byte(0x05);
    irq_byte(0x00);
    CHECK("its packets are a mouse's", take() > 0 && n_got == 3 && got[0].type == EV_KEY &&
                                           got[0].code == BTN_LEFT && got[1].type == EV_REL &&
                                           got[1].code == REL_X && got[1].value == 5);

    model_reset(KIND_PAD);
    refuse_mode = 1;
    CHECK("a touchpad that refuses absolute mode is brought up", init_mouse() == 0);
    CHECK("as a mouse", touchpad == -1 && pad == NULL && mouse_dev != NULL);
    CHECK("the refusal is logged",
          last_warning != 0 &&
              strcmp(last_warning, "mode: the touchpad refused absolute mode") == 0);
    CHECK("the advanced gesture mode is not turned on", !agm_on);
    model_reset(KIND_PAD);
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

/** A Toshiba of Linux's `toshiba_dmi_table`, whose pad reports at 40
 * packets a second, and a Toshiba that is none. */
static void toshiba_rows(void)
{
    model_reset(KIND_PAD);
    set_system("TOSHIBA", "Satellite A200", "PSAE0E");
    CHECK("a Satellite's touchpad is brought up", init_mouse() == 0 && touchpad == 1);
    CHECK("its mode is set at 80 packets a second, then at psmouse's 40",
          modes_set >= 2 && first_mode == 0xC5 && pad_mode == 0x85 && ps2mouse_port_rate() == 40 &&
              reporting);
    synaptics_reconnect();
    CHECK("and its reconnect keeps 40", pad_mode == 0x85);

    model_reset(KIND_PAD);
    set_system("TOSHIBA", "Portable PC", "Version 2.0");
    CHECK("a Portable PC of another version keeps 80",
          init_mouse() == 0 && pad_mode == 0xC5 && ps2mouse_port_rate() == 80);
    set_system(NULL, NULL, NULL);
}

int main(void)
{
    set_system(NULL, NULL, NULL);
    bringup_rows();
    toshiba_rows();
    capability_rows();
    packet_rows();
    mouse_rows();
    written_rows();
    fallback_rows();
    if (fails == 0) {
        printf("synaptics-test: pass\n");
    }
    return fails ? 1 : 0;
}
