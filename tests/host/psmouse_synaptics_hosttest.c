/**
 * The PS/2 port's psmouse files, `protocol`, `rate`, `resolution`,
 * `resetafter` and `resync_time` (sys/arch/x86/ps2mouse.c), read and written
 * as sysfs calls them, on the Lenovo's Synaptics touchpad (synaptics.c),
 * against a model of the 8042 and of that pad copied from
 * synaptics_hosttest.c's, with the pad's packets read from its event device
 * (evdev.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * These are the reads and writes of the Lenovo's trial: each file read,
 * the rate set to 40 and 80, the resolution to 100 and 200, and a packet
 * after each write. The values each row expects are Linux 6.12's: the pad
 * reads SynPS/2, synaptics.c's `synaptics_set_rate` keeps 40 or 80 and
 * sets the mode byte's high-rate bit for 80 and above, psmouse's
 * resolution codes give 100 or 200, and every write runs between the pad's
 * disable and enable, as `psmouse_attr_set_helper` does.
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

#define CHECK(what, cond)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("psmouse-synaptics-test: fail: %s\n", what); \
            fails++;                                            \
        }                                                       \
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

/* The Lenovo's Synaptics touchpad: the command whose argument comes next, 0
 * for none; the byte the resolutions since the last other command make, and
 * their count; its mode byte, resolution, rate and whether it reports; and
 * every byte it took. */
static uint8_t arg_for;
static uint8_t special;
static int res_count;
static uint8_t pad_mode;
static uint8_t dev_res;
static uint8_t dev_rate;
static int reporting;
#define TOOK_MAX 512
static uint8_t took[TOOK_MAX];
static int n_took;

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("psmouse-synaptics-test: fail: the model's output buffer overflowed\n");
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
        if (res_count >= 4 && b == 0x14) {
            pad_mode = special;
        } else if (res_count < 4 || b != 0xC8) {
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
        if (res_count >= 4) {
            answer_query(special);
        } else {
            put3(reporting ? 0x20 : 0x00, dev_res, dev_rate);
        }
        break;
    case 0xEB:
        put3(0x80, 0, 0);
        put3(0xc0, 0, 0);
        break;
    case 0xF2:
        model_put(0x00);
        break;
    case 0xFF:
        model_put(0xAA);
        model_put(0x00);
        pad_mode = 0;
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

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
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
    (void)level;
    (void)subsystem;
    (void)format;
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

/* What the pass-through port's guest and the other pads need, which this
 * pad, without the port, never calls; their own tests test them. */
void k_udelay(uint32_t usec)
{
    (void)usec;
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

/*
 * The files sysfs would hold: each attribute added, the directory it went
 * below, which is 100 and its port's number, and whether a protocol's
 * files or the port's own added it. `evdev_sysfs_serio_driver` and
 * `evdev_sysfs_serio_files` make them at once, as evdev_sysfs.c does for a
 * port whose directory is there.
 */
#define FILES_MAX 64
struct file {
    int dir;
    int of_protocol;
    char name[SYSFS_NAME_MAX];
    struct sysfs_attr attr;
    void *obj;
    int arg;
};
static struct file files[FILES_MAX];
static int n_files;
static int adding_protocol;
static int (*port_driver[3])(int dir);
static int files_made[3]; /* the times a protocol's files were made */

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    struct file *f;

    if (n_files >= FILES_MAX) {
        return -ENOSPC;
    }
    f = &files[n_files++];
    f->dir = parent;
    f->of_protocol = adding_protocol;
    k_strncpy(f->name, attr->name, sizeof(f->name) - 1);
    f->attr = *attr;
    f->obj = obj;
    f->arg = arg;
    return n_files;
}

/** Remove the files below the directory of `port` that a protocol added,
 * `of_protocol` 1, or the port's own, 0. */
static void drop_files(int port, int of_protocol)
{
    int i, n = 0;

    for (i = 0; i < n_files; i++) {
        if (files[i].dir != 100 + port || files[i].of_protocol != of_protocol) {
            files[n++] = files[i];
        }
    }
    n_files = n;
}

/** Remove the guest's captured files and callbacks with its port. */
void evdev_sysfs_serio_remove(int port)
{
    drop_files(port, 0);
    drop_files(port, 1);
    port_driver[port] = NULL;
}

/** This model has no competing command caller. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

void evdev_sysfs_serio_driver(int port, int (*add)(int dir))
{
    port_driver[port] = add;
    drop_files(port, 0);
    if (add) {
        (void)add(100 + port);
    }
}

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    drop_files(port, 1);
    files_made[port]++;
    if (add) {
        adding_protocol = 1;
        (void)add(100 + port);
        adding_protocol = 0;
    }
}

/** The file `name` of the port `port`, or NULL. */
static struct file *file_of(int port, const char *name)
{
    int i;

    for (i = 0; i < n_files; i++) {
        if (files[i].dir == 100 + port && strcmp(files[i].name, name) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

/* The text the last `show` read, with its NUL. */
static char shown[64];

/** Read the file `name` of `port` into `shown`; its length, or -errno. */
static int32_t show(int port, const char *name)
{
    struct file *f = file_of(port, name);
    int32_t n;

    shown[0] = 0;
    if (!f) {
        return -ENOENT;
    }
    n = f->attr.show(f->obj, f->arg, shown, sizeof(shown) - 1);
    if (n >= 0 && n < (int32_t)sizeof(shown)) {
        shown[n] = 0;
    }
    return n;
}

/** Whether the file `name` of `port` reads as `text`. */
static int reads(int port, const char *name, const char *text)
{
    if (show(port, name) < 0 || strcmp(shown, text) != 0) {
        printf("psmouse-synaptics-test: %s of serio%d reads \"%s\", not \"%s\"\n", name, port,
               shown, text);
        return 0;
    }
    return 1;
}

/** Write `text`, no NUL after it, to the file `name` of `port`; what the
 * store returned. */
static int32_t store(int port, const char *name, const char *text)
{
    struct file *f = file_of(port, name);

    if (!f) {
        return -ENOENT;
    }
    return f->attr.store(f->obj, f->arg, text, (uint32_t)k_strlen(text));
}

/** No table is mapped: the SMBIOS reader reads the row's from memory. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return 0;
}

/* An SMBIOS table of its end alone, a machine no quirk names. */
static const uint8_t no_system[] = {127, 4, 0, 0, 0, 0};

/** Put the controller and the pad as the firmware leaves them. */
static void model_reset(void)
{
    model_head = 0;
    model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_res = 2;
    dev_rate = 100;
    n_took = 0;
    n_files = 0;
    smbios_set_table(no_system, sizeof(no_system));
}

/** Whether the pad took the `n` bytes at `want` since `n_took` was
 * cleared; prints what it took when not. */
static int took_bytes(const uint8_t *want, int n)
{
    int i;

    if (n_took != n) {
        printf("psmouse-synaptics-test: the pad took %d bytes, not %d:", n_took, n);
        for (i = 0; i < n_took && i < 32; i++) {
            printf(" %02x", took[i]);
        }
        printf("\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (took[i] != want[i]) {
            printf("psmouse-synaptics-test: byte %d to the pad is 0x%02x, not 0x%02x\n", i, took[i],
                   want[i]);
            return 0;
        }
    }
    return 1;
}

/** Run IRQ 12 once with the pad's byte `b` waiting. */
static void irq_byte(uint8_t b)
{
    model_head = 0;
    model_count = 0;
    model_put(b);
    mouse_irq_handler(0);
}

/** Send one finger at `x`, `y` in a packet of the new absolute form. */
static void send_finger(int x, int y)
{
    irq_byte(0x80 | 0x04 << 2); /* W 4, a finger */
    irq_byte((uint8_t)(((y >> 8) & 0x0f) << 4 | ((x >> 8) & 0x0f)));
    irq_byte(50);
    irq_byte((uint8_t)(0xC0 | ((y >> 12) & 1) << 5 | ((x >> 12) & 1) << 4));
    irq_byte((uint8_t)(x & 0xff));
    irq_byte((uint8_t)(y & 0xff));
}

/** Whether a finger at `x` moves the pad's pointer there: its event
 * device's ABS_X after the packet. */
static int moves_to(int x)
{
    struct input_absinfo ai;

    send_finger(x, 3000);
    k_memset(&ai, 0, sizeof(ai));
    return pad && evdev_get_abs(pad, ABS_X, &ai) == 0 && ai.value == x && model_count == 0;
}

/** The pad brought up, its files, and what each reads. */
static void bringup_rows(void)
{
    model_reset();
    CHECK("the pad is brought up", init_mouse() == 0 && touchpad == TOUCHPAD_SYNAPTICS &&
                                       protocol == PROTO_SYNAPTICS && pad_mode == 0xC5);
    CHECK("serio1 has psmouse's five files and none of the pad's own",
          port_driver[1] == aux_files && files_made[1] == 1 && n_files == 5 &&
              file_of(1, "protocol") && file_of(1, "rate") && file_of(1, "resolution") &&
              file_of(1, "resetafter") && file_of(1, "resync_time"));
    n_took = 0;
    CHECK("protocol reads SynPS/2", reads(1, "protocol", "SynPS/2\n"));
    CHECK("rate reads 80, the pad's high rate", reads(1, "rate", "80\n"));
    CHECK("resolution reads 200", reads(1, "resolution", "200\n"));
    CHECK("resetafter reads 5", reads(1, "resetafter", "5\n"));
    CHECK("resync_time reads 0", reads(1, "resync_time", "0\n"));
    CHECK("the reads send the pad nothing", n_took == 0 && reporting);
    CHECK("a finger moves the pointer", moves_to(3000));
}

/** Writes of `rate`: the mode byte with and without its high-rate bit,
 * between the pad's disable and enable. */
static void rate_rows(void)
{
    static const uint8_t rate40[] = {0xF5, 0xE6, 0xE8, 2,    0xE8, 0,   0xE8,
                                     1,    0xE8, 1,    0xF3, 0x14, 0xF4};
    static const uint8_t rate80[] = {0xF5, 0xE6, 0xE8, 3,    0xE8, 0,   0xE8,
                                     1,    0xE8, 1,    0xF3, 0x14, 0xF4};

    n_took = 0;
    CHECK("40 is written", store(1, "rate", "40") == 2);
    CHECK("as the mode byte 0x85, the high rate off", took_bytes(rate40, 13) && pad_mode == 0x85);
    CHECK("rate reads 40", reads(1, "rate", "40\n"));
    CHECK("the pad reports after it", reporting && state == STATE_ACTIVE);
    CHECK("and a finger moves the pointer", moves_to(3100));
    n_took = 0;
    CHECK("100 is written", store(1, "rate", "100\n") == 4);
    CHECK("as the mode byte 0xC5 again", took_bytes(rate80, 13) && pad_mode == 0xC5);
    CHECK("rate reads 80", reads(1, "rate", "80\n"));
    CHECK("60 reads 40", store(1, "rate", "60") == 2 && reads(1, "rate", "40\n"));
    CHECK("80 reads 80",
          store(1, "rate", "80") == 2 && reads(1, "rate", "80\n") && pad_mode == 0xC5);
    CHECK("a finger moves the pointer", moves_to(3200));
    CHECK("a rate that is no number is refused",
          store(1, "rate", "fast") == -EINVAL && reads(1, "rate", "80\n") && reporting);
}

/** Writes of `resolution`, `resetafter` and `resync_time`. */
static void other_rows(void)
{
    static const uint8_t res100[] = {0xF5, 0xE8, 2, 0xF4};
    static const uint8_t res200[] = {0xF5, 0xE8, 3, 0xF4};
    static const uint8_t frame[] = {0xF5, 0xF4};

    n_took = 0;
    CHECK("resolution 100 is written", store(1, "resolution", "100") == 3);
    CHECK("as psmouse's code 2", took_bytes(res100, 4) && dev_res == 2);
    CHECK("resolution reads 100, the mode kept",
          reads(1, "resolution", "100\n") && pad_mode == 0xC5);
    CHECK("a finger moves the pointer", moves_to(3300));
    n_took = 0;
    CHECK("resolution 200 is written", store(1, "resolution", "200") == 3 &&
                                           took_bytes(res200, 4) &&
                                           reads(1, "resolution", "200\n"));
    CHECK("a finger moves the pointer", moves_to(3400));
    n_took = 0;
    CHECK("resync_time 5 is kept", store(1, "resync_time", "5") == 1 &&
                                       reads(1, "resync_time", "5\n") && took_bytes(frame, 2));
    CHECK("and 0 again", store(1, "resync_time", "0") == 1 && reads(1, "resync_time", "0\n"));
    CHECK("resetafter 3 is kept", store(1, "resetafter", "3") == 1 &&
                                      reads(1, "resetafter", "3\n") &&
                                      store(1, "resetafter", "5") == 1);
    CHECK("a finger moves the pointer", moves_to(3500));
}

/** The switch away from the pad's protocol and back. */
static void switch_rows(void)
{
    CHECK("bare switches the pad to PS/2",
          store(1, "protocol", "bare") == 4 && touchpad == 0 && reads(1, "protocol", "PS/2\n"));
    CHECK("leaving it in relative mode, as synaptics_reset does", pad_mode == 0 && reporting);
    CHECK("synaptics sets the pad up again",
          store(1, "protocol", "synaptics") == 9 && touchpad == TOUCHPAD_SYNAPTICS &&
              reads(1, "protocol", "SynPS/2\n") && pad_mode == 0xC5 && reporting);
    CHECK("a finger moves the pointer", moves_to(3600));
}

/** A partial absolute packet queues a poll through the shared worker. */
static void pause_rows(void)
{
    static const uint8_t resync[] = {0xF5, 0xEB, 0xF4};
    struct evdev *old = pad;

    irq_byte(0x80);
    irq_byte(0);
    kernel_ticks += MS_TO_TICKS(600);
    irq_byte(0);
    CHECK("a pause queues the pad's resync",
          state == STATE_RESYNC && (reconnect_wanted & RESYNC_PORT));
    n_took = 0;
    ps2mouse_resync();
    CHECK("the worker disables, polls six bytes and enables the pad",
          took_bytes(resync, 3) && state == STATE_ACTIVE && reporting && pktcnt == 0 && pad == old);
    CHECK("a finger reports after the poll", moves_to(3700));
}

/** Relative mode is selectable, and a failed absolute setup is retried
 * when the device reconnects. */
static void relative_rows(void)
{
    CHECK("synaptics-relative selects three-byte relative packets",
          store(1, "protocol", "synaptics-relative") == 18 &&
              reads(1, "protocol", "SynRelPS/2\n") && pad_mode == 0 &&
              touchpad == -TOUCHPAD_SYNAPTICS && packet_size == 3 && mouse_dev && !pad);
    CHECK("absolute mode can be selected again",
          store(1, "protocol", "SynPS/2") == 7 && pad && touchpad == TOUCHPAD_SYNAPTICS);
    CHECK("bare leaves absolute mode", store(1, "protocol", "bare") == 4);
    refuse_mode = 1;
    CHECK("a failed absolute setup leaves a relative mouse",
          store(1, "protocol", "synaptics") == 9 && touchpad == -TOUCHPAD_SYNAPTICS && mouse_dev);
    refuse_mode = 0;
    n_took = 0;
    state = STATE_IGNORE;
    reconnect_wanted = RECONNECT_PORT;
    ps2mouse_reconnect();
    CHECK("reconnect tries absolute mode anew after its earlier failure",
          state == STATE_ACTIVE && touchpad == TOUCHPAD_SYNAPTICS && pad && !mouse_dev &&
              pad_mode == 0xc5 && reporting);
}

int main(void)
{
    bringup_rows();
    rate_rows();
    other_rows();
    switch_rows();
    pause_rows();
    relative_rows();
    if (fails == 0) {
        printf("psmouse-synaptics-test: pass\n");
    }
    return fails ? 1 : 0;
}
