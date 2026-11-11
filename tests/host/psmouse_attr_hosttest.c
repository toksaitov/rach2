/**
 * The files of the PS/2 mouse's serio ports, psmouse's `protocol`, `rate`,
 * `resolution`, `resetafter` and `resync_time` and the Elantech pad's
 * registers and settings (sys/arch/x86/ps2mouse.c, elantech.c), read and
 * written as sysfs calls them, against a model of the 8042 and of the
 * device behind its auxiliary port, with the event devices read as a
 * program reads them (evdev.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/park_host.c
 *
 * The driver's files are included whole, so that their static state is
 * reachable. The auxiliary device is a mouse or an Elantech pad. The mouse
 * answers as QEMU's does, an ACK to each command and argument, 0xAA and its
 * ID after a reset, its ID after 0xF2, its status after 0xE9, and a packet
 * of its size after 0xEB, and a row can give it a wheel, which the
 * IntelliMouse's and the Explorer's knocks find; a Kensington
 * ThinkingMouse's ID 2 after psmouse's nine rates; or a Genius mouse's
 * 0x00 0x33 0x55 after three 1:1 scalings. The pad is the model of
 * elantech_hosttest.c cut to three of its pads: a version 1 pad and two
 * version 4 clickpads, one of firmware 0x381f17, which loses its mode at a
 * change of rate. A row can give a mouse to a Synaptics pad's pass-through
 * port, whose bytes go to it through `synaptics_pt`; the Synaptics, ALPS
 * and TrackPoint drivers are stubs here, which their own tests drive.
 *
 * The values each row expects are those Linux 6.12's psmouse-base.c and
 * elantech.c give: psmouse's rate table and resolution codes, its names,
 * which a write must match whole, its protocol switch, which keeps the
 * vendor and name a detect gave, and elantech.c's "0x%02x" of a register,
 * -1 when the read fails, with version 1's mode bits forced on a write.
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2mouse.c"
#include "arch/x86/elantech.c"

#include "fs/fd.h"
#include "lib/kerrno.h"
#include "proc/wait_queue.h"

int printf(const char *format, ...);
int strcmp(const char *a, const char *b);

int park_run(void (*fn)(void));
int park_resume(void);
void park(void);

static int fails;

#define CHECK(what, cond)                                  \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("psmouse-attr-test: fail: %s\n", what); \
            fails++;                                       \
        }                                                  \
    } while (0)

#define MOUSE_BYTE      (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)
#define FIRMWARE_CONFIG 0x65

volatile uint64_t kernel_ticks;

/* A pad of elantech_hosttest.c's table: its replies to the queries 0x01,
 * 0x02, 0x03, 0x00, 0x04 and 0x05, and its hardware version. */
struct profile {
    uint8_t fw[3];
    uint8_t caps[3];
    uint8_t samples[3];
    uint8_t fw_id[3];
    uint8_t res[3];
    uint8_t icbody[3];
    int hw;
};

enum { V1_ROCKER, V4_CLICKPAD, V4_TP500 };

static const struct profile profiles[] = {
    {{0x01, 0x04, 0x00}, {0x04, 0x00, 0x00}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 1},
    {{0x58, 0x1f, 0x01},
     {0x50, 0x17, 0x0d},
     {0x02, 0x0d, 0x64},
     {0x38, 0xa0, 0xfc},
     {0x00, 0x45, 0x00},
     {0, 0, 0},
     4},
    {{0x38, 0x1f, 0x17},
     {0x10, 0x14, 0x0e},
     {0x01, 0x0e, 0x64},
     {0x37, 0x80, 0x84},
     {0x00, 0x55, 0x00},
     {0, 0, 0},
     4},
};

/* The model 8042's output buffer, and its configuration byte and what the
 * next write to port 0x60 is for. */
#define MODEL_BYTES 64
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;
static uint8_t ctl_config;
static uint8_t ctl_next;

/* What the auxiliary device is. */
#define KIND_MOUSE 1
#define KIND_PAD   2
static int dev_kind;

/*
 * A mouse, on the auxiliary port or behind the pass-through port: the
 * highest ID its wheel answers and the ID it answers now; how far into a
 * knock its rates have gone; whether it is a ThinkingMouse or a Genius
 * mouse, its last nine rates and the 1:1 scalings after resolution 3; the
 * command whose argument comes next; its resolution code, rate and
 * reporting; whether it leaves 0xEB unanswered; and the bytes it took.
 */
#define TOOK_MAX 1024
struct mouse {
    int wheel;
    int id;
    int knock;
    int thinking;
    int genius;
    uint8_t rates[9];
    int genius_scales;
    uint8_t arg_for;
    uint8_t res;
    uint8_t rate;
    int reporting;
    int poll_silent;
    uint8_t took[TOOK_MAX];
    int n_took;
};
static struct mouse aux_mouse;
static struct mouse pt_mouse;

/* The guest's replies, which `synaptics_pt` reads, and whether a guest is
 * behind the pass-through port. */
static uint8_t pt_data[MODEL_BYTES];
static int pt_head;
static int pt_count;
static int pt_present;

/* The pad of `pro`, as elantech_hosttest.c models it: the sliced and 0xF8
 * sequences since the last other command, the 1:1 scalings in a row, its
 * registers, register 0x07's second byte, the register whose write it
 * leaves unanswered (-1 for none), and its reporting. */
static const struct profile *pro;
static uint8_t arg_for;
static uint8_t special;
static int res_count;
static uint8_t sliced_seq[4];
static int n_sliced;
static uint8_t custom_seq[4];
static int n_custom;
static int custom_next;
static int scales;
static uint8_t regs[0x30];
static uint8_t reg07_hi;
static uint8_t dev_res;
static uint8_t dev_rate;
static int reporting;
static int refuse_reg;
static uint8_t took[TOOK_MAX];
static int n_took;

/* The Synaptics stub's answer to its probe, 1 for a pad with a
 * pass-through port, and the rates its `set_rate` was given. */
static int synaptics_answer;
static unsigned int synaptics_rate;

/* Interrupts and the warnings logged. */
static int irq_on = 1;
static int warnings;
static const char *last_warning;

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("psmouse-attr-test: fail: the model's output buffer overflowed\n");
        fails++;
        return;
    }
    model_data[model_head + model_count] = data;
    model_count++;
}

/** Put `data` behind the guest's replies. */
static void pt_put(uint8_t data)
{
    if (pt_count == 0) {
        pt_head = 0;
    }
    if (pt_head + pt_count < MODEL_BYTES) {
        pt_data[pt_head + pt_count] = data;
        pt_count++;
    }
}

/** Put the mouse `m`'s reply byte `b` where its port reads it. */
static void mouse_put(struct mouse *m, uint8_t b)
{
    if (m == &pt_mouse) {
        pt_put(b);
    } else {
        model_put(b);
    }
}

/** Take the rate `rate`, which moves a knock on as QEMU's mouse does. */
static void mouse_knock(struct mouse *m, uint8_t rate)
{
    switch (m->knock) {
    case 0:
        m->knock = rate == 200 ? 1 : 0;
        break;
    case 1:
        m->knock = rate == 100 ? 2 : rate == 200 ? 3 : 0;
        break;
    case 2:
        if (rate == 80 && m->wheel >= ID_IMPS) {
            m->id = ID_IMPS;
        }
        m->knock = 0;
        break;
    default:
        if (rate == 80 && m->wheel >= ID_IMEX) {
            m->id = ID_IMEX;
        }
        m->knock = 0;
        break;
    }
}

/** Whether the mouse's last nine rates are psmouse's ThinkingMouse's. */
static int thinking_rates(const struct mouse *m)
{
    static const uint8_t seq[9] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
    int i;

    for (i = 0; i < 9; i++) {
        if (m->rates[i] != seq[i]) {
            return 0;
        }
    }
    return 1;
}

/** Take `b`, a byte sent to the mouse `m`. */
static void mouse_take(struct mouse *m, uint8_t b)
{
    int i;

    if (m->n_took < TOOK_MAX) {
        m->took[m->n_took] = b;
    }
    m->n_took++;
    mouse_put(m, 0xFA);
    if (m->arg_for) {
        if (m->arg_for == 0xF3) {
            m->rate = b;
            mouse_knock(m, b);
            for (i = 0; i < 8; i++) {
                m->rates[i] = m->rates[i + 1];
            }
            m->rates[8] = b;
        } else {
            m->res = b;
            m->genius_scales = b == 3 ? 0 : -1;
        }
        m->arg_for = 0;
        return;
    }
    switch (b) {
    case 0xE8:
    case 0xF3:
        m->arg_for = b;
        return;
    case 0xE6:
        if (m->genius_scales >= 0) {
            m->genius_scales++;
        }
        return;
    case 0xE9:
        if (m->genius && m->genius_scales == 3) {
            mouse_put(m, 0x00);
            mouse_put(m, 0x33);
            mouse_put(m, 0x55);
        } else {
            mouse_put(m, m->reporting ? 0x20 : 0x00);
            mouse_put(m, m->res);
            mouse_put(m, m->rate);
        }
        break;
    case 0xEB:
        if (!m->poll_silent) {
            mouse_put(m, 0x08);
            mouse_put(m, 0x00);
            mouse_put(m, 0x00);
            if (m->id) {
                mouse_put(m, 0x00);
            }
        }
        break;
    case 0xF2:
        mouse_put(m, m->thinking && thinking_rates(m) ? 2 : (uint8_t)m->id);
        break;
    case 0xFF:
        mouse_put(m, 0xAA);
        mouse_put(m, 0x00);
        m->id = 0;
        m->knock = 0;
        m->reporting = 0;
        m->res = 2;
        m->rate = 100;
        break;
    case 0xF4:
        m->reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        m->reporting = 0;
        break;
    default:
        break;
    }
    m->genius_scales = -1;
}

/** Put the three bytes at `p` in the output buffer. */
static void put3(const uint8_t *p)
{
    model_put(p[0]);
    model_put(p[1]);
    model_put(p[2]);
}

/** Answer the query `q` from the pad's profile. */
static void answer_query(uint8_t q)
{
    static const uint8_t zero[3] = {0, 0, 0};

    switch (q) {
    case 0x00:
        put3(pro->fw_id);
        break;
    case 0x01:
        put3(pro->fw);
        break;
    case 0x02:
        put3(pro->caps);
        break;
    case 0x03:
        put3(pro->samples);
        break;
    case 0x04:
        put3(pro->res);
        break;
    case 0x05:
        put3(pro->icbody);
        break;
    default:
        put3(zero);
        break;
    }
}

/** Answer a read of register `reg`: its value in the first byte, or on
 * version 4 in the second. */
static void answer_reg(uint8_t reg)
{
    uint8_t r[3] = {0, 0, 0};
    uint8_t val = reg < sizeof(regs) ? regs[reg] : 0;

    if (pro->hw == 4) {
        r[0] = reg == 0x07 ? val : 0;
        r[1] = reg == 0x07 ? reg07_hi : val;
    } else {
        r[0] = val;
    }
    put3(r);
}

/** The status request: the magic knock's reply, a register's or a query's
 * after their sequences, or the pad's status. */
static void answer_status(void)
{
    static const uint8_t magic[3] = {0x3c, 0x03, 0xc8};
    uint8_t status[3];

    model_put(0xFA);
    if (n_custom == 2 && custom_seq[0] == 0x00) {
        answer_reg(custom_seq[1]);
    } else if (n_sliced == 2 && sliced_seq[0] == 0x10) {
        answer_reg(sliced_seq[1]);
    } else if (n_custom == 1) {
        answer_query(custom_seq[0]);
    } else if (n_sliced == 1 && n_custom == 0) {
        answer_query(sliced_seq[0]);
    } else if (scales == 3) {
        put3(magic);
    } else {
        status[0] = reporting ? 0x20 : 0x00;
        status[1] = dev_res;
        status[2] = dev_rate;
        put3(status);
    }
}

/** Write `val` to register `reg`, unless the row refuses it; -1 then. */
static int write_reg(uint8_t reg, uint8_t val)
{
    if (reg == refuse_reg) {
        return -1;
    }
    if (reg < sizeof(regs)) {
        regs[reg] = val;
    }
    return 0;
}

/** The 1:1 scaling that ends a register write of the pad's version; -1
 * for a write the model leaves unanswered. */
static int end_write(void)
{
    if (n_sliced == 3 && sliced_seq[0] == 0x11) {
        return write_reg(sliced_seq[1], sliced_seq[2]);
    }
    if (n_custom == 4 && custom_seq[0] == 0x00 && pro->hw == 4) {
        if (custom_seq[2] == 0x00) {
            return write_reg(custom_seq[1], custom_seq[3]);
        }
        reg07_hi = custom_seq[3];
        return write_reg(custom_seq[1], custom_seq[2]);
    }
    return 0;
}

/** Forget the sequences of a register access or a query. */
static void end_sequences(void)
{
    n_sliced = 0;
    n_custom = 0;
    res_count = 0;
    special = 0;
}

/** Take `b`, a byte sent to the pad. */
static void pad_take(uint8_t b)
{
    uint8_t cmd;

    if (custom_next) {
        custom_next = 0;
        if (n_custom < 4) {
            custom_seq[n_custom++] = b;
        }
        model_put(0xFA);
        return;
    }
    if (arg_for) {
        cmd = arg_for;
        arg_for = 0;
        model_put(0xFA);
        if (cmd == 0xE8) {
            special = (uint8_t)(special << 2 | (b & 3u));
            dev_res = b;
            if (++res_count == 4) {
                if (n_sliced < 4) {
                    sliced_seq[n_sliced++] = special;
                }
                res_count = 0;
                special = 0;
            }
        } else {
            dev_rate = b;
        }
        return;
    }
    if (b == 0xE9) {
        answer_status();
        end_sequences();
        scales = 0;
        return;
    }
    if (b == 0xE6) {
        if (end_write() < 0) {
            return; /* refused: no answer, and the sequence stands */
        }
        model_put(0xFA);
        if (n_sliced == 3 || n_custom >= 3) {
            end_sequences();
        }
        res_count = 0;
        special = 0;
        scales++;
        return;
    }
    model_put(0xFA);
    scales = 0;
    switch (b) {
    case 0xE8:
    case 0xF3:
        arg_for = b;
        return;
    case 0xF8:
        custom_next = 1;
        return;
    case 0xF2:
        model_put(0x00);
        break;
    case 0xFF:
        model_put(0xAA);
        model_put(0x00);
        k_memset(regs, 0, sizeof(regs));
        reg07_hi = 0;
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
    end_sequences();
}

/** Take `b`, a byte the controller sent on to the auxiliary device. */
static void dev_take(uint8_t b)
{
    if (n_took < TOOK_MAX) {
        took[n_took] = b;
    }
    n_took++;
    if ((ctl_config & 0x20) != 0) {
        return; /* its clock is off */
    }
    if (dev_kind == KIND_PAD) {
        pad_take(b);
    } else {
        mouse_take(&aux_mouse, b);
    }
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

/** Send `cmd` to the guest; its reply, or -1 when none is there. */
static int pt_send(uint8_t cmd)
{
    if (!pt_present) {
        return -1;
    }
    mouse_take(&pt_mouse, cmd);
    pt_count--;
    return pt_data[pt_head++];
}

/** The guest's next reply, or -1. */
static int pt_receive(void)
{
    if (pt_count == 0) {
        return -1;
    }
    pt_count--;
    return pt_data[pt_head++];
}

const struct ps2io synaptics_pt = {pt_send, pt_receive};

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
    (void)handler;
    (void)name;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
        last_warning = format;
    }
}

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return 0;
}

void console_poke(void)
{}

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

/** Remove the captured files and callbacks with the guest's port. */
void evdev_sysfs_serio_remove(int port)
{
    drop_files(port, 0);
    drop_files(port, 1);
    port_driver[port] = NULL;
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
        printf("psmouse-attr-test: %s of serio%d reads \"%s\", not \"%s\"\n", name, port, shown,
               text);
        return 0;
    }
    return 1;
}

/** Write all `len` bytes of `text` to a captured attribute. */
static int32_t store_bytes(int port, const char *name, const char *text, uint32_t len)
{
    struct file *f = file_of(port, name);

    if (!f) {
        return -ENOENT;
    }
    return f->attr.store(f->obj, f->arg, text, len);
}

/** Write `text`, without its NUL, to the file `name` of `port`. */
static int32_t store(int port, const char *name, const char *text)
{
    return store_bytes(port, name, text, (uint32_t)k_strlen(text));
}

/* The Synaptics driver, a stub: a pad with a pass-through port when the row
 * says, which takes no bytes of its own. */
int synaptics_init(void)
{
    return synaptics_answer;
}

int synaptics_byte(uint8_t data)
{
    (void)data;
    return SYNAPTICS_GOOD;
}

void synaptics_packet_restart(void)
{}

int synaptics_packet_start(void)
{
    return 1;
}

int synaptics_has_pass_through(void)
{
    return synaptics_answer > 0;
}

void synaptics_pt_activate(int packet_size)
{
    (void)packet_size;
}

int synaptics_reconnect(void)
{
    return 0;
}

void synaptics_disconnect(void)
{}

unsigned int synaptics_set_rate(unsigned int rate)
{
    synaptics_rate = rate;
    return rate >= 80 ? 80 : 40;
}

/* The TrackPoint driver: no stick answers. */
int trackpoint_detect(const struct ps2io *io)
{
    (void)io;
    return -1;
}

int trackpoint_identify(const struct ps2io *io)
{
    (void)io;
    return -1;
}

const char *trackpoint_vendor(void)
{
    return "";
}

int trackpoint_buttons(void)
{
    return 3;
}

int trackpoint_reconnect(const struct ps2io *io)
{
    (void)io;
    return -1;
}

int trackpoint_sysfs_add(int dir)
{
    (void)dir;
    return 0;
}

/* The ALPS driver: no ALPS pad answers. */
int alps_init(void)
{
    return 0;
}

int alps_byte(uint8_t data)
{
    (void)data;
    return ALPS_GOOD;
}

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

const char *alps_mouse_name(const char *proto)
{
    return proto;
}

int alps_poll(uint8_t badbyte, uint8_t *buf)
{
    (void)badbyte;
    (void)buf;
    return -1;
}

/* What evdev.c needs: one processor, fixed clocks, reads that never sleep,
 * and calls of x86-64 programs. */
int in_compat_syscall(void)
{
    return 0;
}

uint32_t kirq_save(void)
{
    uint32_t flags = (uint32_t)irq_on;

    irq_on = 0;
    return flags;
}

void kirq_restore(uint32_t flags)
{
    irq_on = (int)flags;
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

/** Sleep a competing callback on its own stack until the command is free. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    while (!cond(arg)) {
        park();
    }
    return 0;
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

#define IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('E' << 8) | (nr))
#define EVIOCGNAME(n)      IOC(2, 0x06, n)

/** How many event devices are named `name`; every one it opens it closes. */
static int count_named(const char *name)
{
    fd_entry_t e;
    struct open_file ofd;
    char text[64];
    int n, count = 0;

    for (n = 0; n < EVDEV_MAX; n++) {
        k_memset(&e, 0, sizeof(e));
        k_memset(&ofd, 0, sizeof(ofd));
        ofd.status_flags = OFD_NONBLOCK;
        e.ofd = &ofd;
        if (evdev_open(&e, n) != 0) {
            continue;
        }
        if (evdev_ioctl(&e, EVIOCGNAME(64), (uintptr_t)text) > 0 && strcmp(text, name) == 0) {
            count++;
        }
        evdev_close(&e);
    }
    return count;
}

/* A nonblocking event descriptor and the records its reads return. */
struct opened {
    fd_entry_t e;
    struct open_file ofd;
};
struct event {
    unsigned long sec, usec;
    uint16_t type, code;
    int32_t value;
};

/** Open the current event device of `phys`; return 0, or -ENODEV. */
static int open_phys(struct opened *o, const char *phys)
{
    struct evdev_desc d;
    int n;

    k_memset(o, 0, sizeof(*o));
    o->ofd.status_flags = OFD_NONBLOCK;
    o->e.ofd = &o->ofd;
    for (n = 0; n < EVDEV_MAX; n++) {
        if (evdev_snapshot(n, &d) && strcmp(d.phys, phys) == 0) {
            return evdev_open(&o->e, n);
        }
    }
    return -ENODEV;
}

/** Whether a packet read from `o` contains the event specified. */
static int has_event(struct opened *o, uint16_t type, uint16_t code, int32_t value)
{
    struct event events[16];
    int32_t bytes;
    int n;

    bytes = evdev_read(&o->e, (char *)events, sizeof(events));
    for (n = 0; bytes > 0 && n < bytes / (int32_t)sizeof(events[0]); n++) {
        if (events[n].type == type && events[n].code == code && events[n].value == value) {
            return 1;
        }
    }
    return 0;
}

static const uint8_t no_system[] = {127, 4, 0, 0, 0, 0};

/** A mouse as the firmware leaves it, with no wheel and no quirk. */
static void mouse_clear(struct mouse *m)
{
    k_memset(m, 0, sizeof(*m));
    m->res = 2;
    m->rate = 100;
    m->genius_scales = -1;
}

/** Take away what the last row registered, and put the controller and the
 * device as the firmware leaves them: a device of `kind`, with the pad
 * `p` for KIND_PAD. */
static void model_reset(int kind, int p)
{
    if (touchpad == TOUCHPAD_ELANTECH || touchpad == -TOUCHPAD_ELANTECH) {
        elantech_disconnect();
    }
    guest_forget();
    evdev_unregister(mouse_dev);
    mouse_dev = NULL;
    touchpad = 0;
    alps = 0;
    state = STATE_ACTIVE;
    bad_in_a_row = 0;
    bat_held = 0;
    reconnect_wanted = 0;
    cmd_busy = 0;
    packet_index = 0;
    model_head = 0;
    model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_kind = kind;
    pro = &profiles[p];
    smbios_set_table(no_system, sizeof(no_system));
    mouse_clear(&aux_mouse);
    mouse_clear(&pt_mouse);
    pt_head = 0;
    pt_count = 0;
    pt_present = 0;
    arg_for = 0;
    special = 0;
    res_count = 0;
    n_sliced = 0;
    n_custom = 0;
    custom_next = 0;
    scales = 0;
    k_memset(regs, 0, sizeof(regs));
    reg07_hi = 0;
    dev_res = 2;
    dev_rate = 100;
    reporting = 0;
    refuse_reg = -1;
    n_took = 0;
    synaptics_answer = 0;
    synaptics_rate = 0;
    warnings = 0;
    last_warning = 0;
    n_files = 0;
    k_memset(files_made, 0, sizeof(files_made));
    k_memset(port_driver, 0, sizeof(port_driver));
    etd_pktcnt = 0;
}

/** Whether the auxiliary device took the `n` bytes at `want` since
 * `n_took` was cleared; prints the first that differs. */
static int took_bytes(const uint8_t *want, int n)
{
    int i;

    if (n_took != n) {
        printf("psmouse-attr-test: the device took %d bytes, not %d:", n_took, n);
        for (i = 0; i < n_took && i < 32; i++) {
            printf(" %02x", took[i]);
        }
        printf("\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (took[i] != want[i]) {
            printf("psmouse-attr-test: byte %d to the device is 0x%02x, not 0x%02x\n", i, took[i],
                   want[i]);
            return 0;
        }
    }
    return 1;
}

/** Feed `data` to the IRQ 12 handler as the mouse's byte. */
static void irq_byte(uint8_t data)
{
    model_put(data);
    mouse_irq_handler(0);
}

/** The port files of a plain mouse: their names and what each reads after
 * the bring-up. */
static void plain_rows(void)
{
    model_reset(KIND_MOUSE, 0);
    CHECK("a plain mouse is brought up", init_mouse() == 0 && protocol == PROTO_PS2);
    CHECK("serio1 has psmouse's five files, made by the port's driver",
          port_driver[1] == aux_files && file_of(1, "protocol") && file_of(1, "rate") &&
              file_of(1, "resolution") && file_of(1, "resetafter") && file_of(1, "resync_time") &&
              n_files == 5);
    CHECK("each mode 0644 with a show and a store", file_of(1, "rate")->attr.mode == 0644u &&
                                                        file_of(1, "rate")->attr.show &&
                                                        file_of(1, "rate")->attr.store);
    CHECK("protocol reads PS/2", reads(1, "protocol", "PS/2\n"));
    CHECK("rate reads 100", reads(1, "rate", "100\n"));
    CHECK("resolution reads 200", reads(1, "resolution", "200\n"));
    CHECK("resetafter reads 5", reads(1, "resetafter", "5\n"));
    CHECK("resync_time reads 0", reads(1, "resync_time", "0\n"));
    CHECK("the reads send nothing", aux_mouse.reporting && model_count == 0);
}

/** Writes of `rate` to the plain mouse: the bytes each sends, psmouse's
 * rate table, and kstrtouint's refusals. */
static void rate_rows(void)
{
    static const uint8_t rate60[] = {0xF5, 0xF3, 60, 0xF4};
    static const uint8_t rate40[] = {0xF5, 0xF3, 40, 0xF4};
    static const uint8_t rate10[] = {0xF5, 0xF3, 10, 0xF4};
    static const uint8_t rate0[] = {0xF5, 0xF3, 0, 0xF4};
    static const uint8_t frame[] = {0xF5, 0xF4};

    n_took = 0;
    CHECK("a write of 60 to rate is taken whole", store(1, "rate", "60\n") == 3);
    CHECK("with the mouse disabled around the rate", took_bytes(rate60, 4));
    CHECK("which reads 60 after", reads(1, "rate", "60\n") && aux_mouse.rate == 60);
    CHECK("and the mouse reports", aux_mouse.reporting && state == STATE_ACTIVE);
    n_took = 0;
    CHECK("55 sets the next rate below it, 40",
          store(1, "rate", "55") == 2 && took_bytes(rate40, 4) && reads(1, "rate", "40\n"));
    n_took = 0;
    CHECK("15 sets 10", store(1, "rate", "15") == 2 && took_bytes(rate10, 4));
    n_took = 0;
    CHECK("9 sets psmouse's last rate, 0",
          store(1, "rate", "9") == 1 && took_bytes(rate0, 4) && reads(1, "rate", "0\n"));
    n_took = 0;
    CHECK("a rate that is no number is refused", store(1, "rate", "fast") == -EINVAL);
    CHECK("with the mouse disabled and enabled, as psmouse's helper does", took_bytes(frame, 2));
    CHECK("a negative rate is refused", store(1, "rate", "-1") == -EINVAL);
    CHECK("so is a rate past 32 bits", store(1, "rate", "4294967296") == -ERANGE);
    CHECK("and a second newline", store(1, "rate", "100\n\n") == -EINVAL);
    CHECK("a '+' is taken", store(1, "rate", "+200") == 4 && reads(1, "rate", "200\n"));
}

/** Writes of `resolution`: psmouse's codes and the counts an inch each
 * reads as. */
static void resolution_rows(void)
{
    static const uint8_t res100[] = {0xF5, 0xE8, 2, 0xF4};
    static const uint8_t res200[] = {0xF5, 0xE8, 3, 0xF4};
    static const uint8_t res25[] = {0xF5, 0xE8, 0, 0xF4};

    n_took = 0;
    CHECK("a write of 100 to resolution sends code 2",
          store(1, "resolution", "100\n") == 4 && took_bytes(res100, 4));
    CHECK("and reads 100", reads(1, "resolution", "100\n") && aux_mouse.res == 2);
    n_took = 0;
    CHECK("0 is 200", store(1, "resolution", "0") == 1 && took_bytes(res200, 4) &&
                          reads(1, "resolution", "200\n"));
    n_took = 0;
    CHECK("300 is 200", store(1, "resolution", "300") == 3 && took_bytes(res200, 4));
    n_took = 0;
    CHECK("49 is 25", store(1, "resolution", "49") == 2 && took_bytes(res25, 4) &&
                          reads(1, "resolution", "25\n"));
    CHECK("150 is 100, psmouse's code 2",
          store(1, "resolution", "150") == 3 && reads(1, "resolution", "100\n"));
    CHECK("75 is 50", store(1, "resolution", "75") == 2 && reads(1, "resolution", "50\n"));
}

/** `resetafter` on a plain mouse: the bad packets that ask for a
 * reconnect, the writes refused while it is due, and 0, which never asks
 * one. */
static void resetafter_rows(void)
{
    int i;

    model_reset(KIND_MOUSE, 0);
    CHECK("a mouse is brought up", init_mouse() == 0);
    n_took = 0;
    CHECK("resetafter takes 3", store(1, "resetafter", "3") == 1 && reads(1, "resetafter", "3\n"));
    CHECK("and sends only the disable and enable", n_took == 2);
    for (i = 0; i < 2; i++) {
        irq_byte(0x00);
    }
    CHECK("two bad packets ask nothing", bad_in_a_row == 2 && !reconnect_wanted);
    irq_byte(0x00);
    CHECK("the third asks for a reconnect",
          state == STATE_IGNORE && (reconnect_wanted & RECONNECT_PORT));
    CHECK("a write while the port waits for it is -ENODEV, sending nothing",
          (n_took = 0, store(1, "rate", "40")) == -ENODEV && n_took == 0);
    CHECK("as is a write of resetafter", store(1, "resetafter", "5") == -ENODEV);
    CHECK("the files still read", reads(1, "resetafter", "3\n"));
    ps2mouse_reconnect();
    CHECK("the reconnect keeps the port's settings",
          state == STATE_ACTIVE && reads(1, "resetafter", "3\n") && reads(1, "rate", "100\n"));

    model_reset(KIND_MOUSE, 0);
    CHECK("a mouse is brought up again", init_mouse() == 0);
    CHECK("resetafter 0 never reconnects", store(1, "resetafter", "0") == 1);
    for (i = 0; i < 20; i++) {
        irq_byte(0x00);
    }
    CHECK("twenty bad packets ask nothing", !reconnect_wanted && state == STATE_ACTIVE);
}

/** `resync_time`: the idle time after which a first byte begins a resync,
 * which polls the mouse, and a poll left unanswered, which asks for a
 * reconnect. */
static void resync_time_rows(void)
{
    static const uint8_t resync[] = {0xF5, 0xEB, 0xF4};

    CHECK("resync_time takes 2 on a mouse of three bytes",
          store(1, "resync_time", "2") == 1 && reads(1, "resync_time", "2\n"));
    irq_byte(0x08);
    irq_byte(0x00);
    irq_byte(0x00);
    CHECK("a packet after it is taken", !reconnect_wanted && packet_index == 0);
    kernel_ticks += MS_TO_TICKS(2000);
    irq_byte(0x08);
    CHECK("a first byte two seconds idle is taken", !reconnect_wanted);
    irq_byte(0x00);
    irq_byte(0x00);
    kernel_ticks += MS_TO_TICKS(2001);
    irq_byte(0x08);
    CHECK("a first byte past two seconds idle begins a resync",
          state == STATE_RESYNC && (reconnect_wanted & RESYNC_PORT) && badbyte == 0x08);
    n_took = 0;
    ps2mouse_resync();
    CHECK("the resync disables, polls and enables the mouse", took_bytes(resync, 3));
    CHECK("and the port is active again",
          state == STATE_ACTIVE && !reconnect_wanted && aux_mouse.reporting && packet_index == 0);

    kernel_ticks += MS_TO_TICKS(3000);
    aux_mouse.poll_silent = 1;
    irq_byte(0x08);
    ps2mouse_resync();
    CHECK("a resync whose poll is not answered asks for a reconnect",
          state == STATE_IGNORE && (reconnect_wanted & RECONNECT_PORT));
    aux_mouse.poll_silent = 0;
    ps2mouse_reconnect();
    CHECK("whose reconnect keeps resync_time, as psmouse's does",
          state == STATE_ACTIVE && reads(1, "resync_time", "2\n"));
}

/** The switch of protocol's rule for `resync_time`: kept on a mouse of
 * four bytes that answers a poll, zeroed on one that does not and for a
 * packet of three bytes. */
static void resync_switch_rows(void)
{
    model_reset(KIND_MOUSE, 0);
    aux_mouse.wheel = ID_IMEX;
    CHECK("an Explorer is brought up", init_mouse() == 0 && protocol == PROTO_IMEX);
    CHECK("a protocol written keeps resync_time on a mouse that answers a poll",
          store(1, "resync_time", "1") == 1 && store(1, "protocol", "imps") == 4 &&
              reads(1, "resync_time", "1\n"));
    n_took = 0;
    CHECK("the protocol it speaks already sends only the disable and enable",
          store(1, "protocol", "imps") == 4 && n_took == 2);
    aux_mouse.poll_silent = 1;
    CHECK("another zeroes it on a mouse that does not answer a poll",
          store(1, "protocol", "exps") == 4 && protocol == PROTO_IMEX &&
              reads(1, "resync_time", "0\n"));
    CHECK("bare zeroes it, a packet of three bytes", store(1, "resync_time", "1") == 1 &&
                                                         store(1, "protocol", "bare") == 4 &&
                                                         reads(1, "resync_time", "0\n"));
}

/** `protocol`: the names psmouse takes, matched whole, the switch to each
 * with its new event device, and the fallback to PS/2 when the device does
 * not speak the protocol written. */
static void protocol_rows(void)
{
    model_reset(KIND_MOUSE, 0);
    aux_mouse.wheel = ID_IMEX;
    CHECK("an Explorer is brought up", init_mouse() == 0 && protocol == PROTO_IMEX);
    CHECK("protocol reads ImExPS/2", reads(1, "protocol", "ImExPS/2\n"));
    CHECK("its device is ImExPS/2 Generic Explorer Mouse",
          count_named("ImExPS/2 Generic Explorer Mouse") == 1);
    CHECK("a name with a newline is refused, as psmouse matches the bytes written",
          store(1, "protocol", "imps\n") == -EINVAL);
    CHECK("a name psmouse does not know is refused", store(1, "protocol", "mouse") == -EINVAL);
    CHECK("so is one Rach2 has no detect for",
          store(1, "protocol", "lifebook") == -EINVAL && store(1, "protocol", "PS2++") == -EINVAL);
    CHECK("a protocol write keeps its embedded NUL in the length",
          store_bytes(1, "protocol", "bare\0junk", 9) == -EINVAL);
    CHECK("a refused name leaves the protocol", protocol == PROTO_IMEX && aux_mouse.reporting);
    CHECK("the alias imps is taken", store(1, "protocol", "imps") == 4);
    CHECK("and the mouse speaks ImPS/2",
          protocol == PROTO_IMPS && packet_size == 4 && reads(1, "protocol", "ImPS/2\n"));
    CHECK("on a new device, the old one gone, keeping the name the Explorer's detect gave",
          count_named("ImPS/2 Generic Explorer Mouse") == 1 &&
              count_named("ImExPS/2 Generic Explorer Mouse") == 0);
    CHECK("the files stay", file_of(1, "rate") != NULL && aux_mouse.reporting);
    CHECK("the name PS/2 is taken", store(1, "protocol", "PS/2") == 4);
    CHECK("and gives PS/2 Generic Explorer Mouse",
          protocol == PROTO_PS2 && packet_size == 3 &&
              count_named("PS/2 Generic Explorer Mouse") == 1 &&
              count_named("ImPS/2 Generic Explorer Mouse") == 0);
}

/** More switches: the rate kept across one, Cortron's, which has no
 * detect, the fallback to PS/2 that keeps the name a detect gave, and
 * `any`, which probes the port whole. */
static void switch_rows(void)
{
    CHECK("exps finds the Explorer again",
          store(1, "protocol", "exps") == 4 && protocol == PROTO_IMEX && aux_mouse.id == ID_IMEX);
    CHECK("the rate written before a switch is kept",
          store(1, "rate", "40") == 2 && store(1, "protocol", "imps") == 4 &&
              aux_mouse.rate == 40 && reads(1, "rate", "40\n"));
    CHECK("cortps is taken without a detect, as psmouse's has none",
          store(1, "protocol", "cortps") == 6 && protocol == PROTO_CORTRON &&
              count_named("CortronPS/2 Cortron PS/2 Trackball") == 1);
    CHECK("thinkps on a mouse that is no ThinkingMouse falls back to PS/2",
          store(1, "protocol", "thinkps") == 7 && protocol == PROTO_PS2 &&
              reads(1, "protocol", "PS/2\n"));
    CHECK("keeping the name the Cortron detect gave, as psmouse keeps its vendor",
          count_named("PS/2 Cortron PS/2 Trackball") == 1);
    CHECK("synaptics on a mouse falls back to PS/2 too",
          store(1, "protocol", "synaptics") == 9 && protocol == PROTO_PS2 && aux_mouse.reporting);
    CHECK("after the reset psmouse sends", aux_mouse.id == 0);
    CHECK("any probes the port whole", store(1, "protocol", "any") == 3 && protocol == PROTO_IMEX &&
                                           reads(1, "protocol", "ImExPS/2\n"));
    CHECK("no switch warns", warnings == 0);
}

/** The ThinkingMouse and the Genius mouse, which psmouse's probe finds and
 * their names find again. */
static void detect_rows(void)
{
    static const uint8_t thinking[] = {0xF5, 0xF3, 10, 0xE8, 0,    0xF3, 20,   0xF3, 60,   0xF3,
                                       40,   0xF3, 20, 0xF3, 20,   0xF3, 60,   0xF3, 40,   0xF3,
                                       20,   0xF3, 20, 0xF2, 0xF3, 100,  0xE8, 3,    0xE6, 0xF4};
    static const uint8_t genius[] = {0xF5, 0xE8, 3,    0xE6, 0xE6, 0xE6, 0xE9,
                                     0xF3, 100,  0xE8, 3,    0xE6, 0xF4};
    struct opened o;

    model_reset(KIND_MOUSE, 0);
    aux_mouse.thinking = 1;
    CHECK("a ThinkingMouse is found first", init_mouse() == 0 && protocol == PROTO_THINKPS);
    CHECK("protocol reads ThinkPS/2", reads(1, "protocol", "ThinkPS/2\n"));
    CHECK("its device is ThinkPS/2 Kensington ThinkingMouse",
          count_named("ThinkPS/2 Kensington ThinkingMouse") == 1);
    CHECK("bare leaves the detected ThinkingMouse protocol",
          store(1, "protocol", "bare") == 4 && protocol == PROTO_PS2);
    n_took = 0;
    CHECK("thinkps detects it again",
          store(1, "protocol", "thinkps") == 7 && protocol == PROTO_THINKPS);
    CHECK("the forced detect sends the nine rates and reads the ID",
          took_bytes(thinking, sizeof(thinking)));
    CHECK("the new ThinkingMouse device opens", open_phys(&o, "isa0060/serio1/input0") == 0);
    irq_byte(0x48);
    irq_byte(1);
    irq_byte(0);
    CHECK("its overflow bit supplies the high bit of X", has_event(&o, EV_REL, REL_X, 129));
    evdev_close(&o.e);

    model_reset(KIND_MOUSE, 0);
    aux_mouse.genius = 1;
    CHECK("a Genius mouse is found", init_mouse() == 0 && protocol == PROTO_GENPS);
    CHECK("protocol reads GenPS/2", reads(1, "protocol", "GenPS/2\n"));
    CHECK("its device is GenPS/2 Genius Mouse, of four bytes",
          count_named("GenPS/2 Genius Mouse") == 1 && packet_size == 4);
    CHECK("bare leaves the detected Genius protocol",
          store(1, "protocol", "bare") == 4 && protocol == PROTO_PS2);
    n_took = 0;
    CHECK("genius detects it again",
          store(1, "protocol", "genius") == 6 && protocol == PROTO_GENPS);
    CHECK("the forced detect sends three scalings and reads status",
          took_bytes(genius, sizeof(genius)));
    CHECK("the new Genius device opens", open_phys(&o, "isa0060/serio1/input0") == 0);
    irq_byte(0x08);
    irq_byte(0);
    irq_byte(0);
    irq_byte(0xff);
    CHECK("its fourth byte reports a wheel detent", has_event(&o, EV_REL, REL_WHEEL, 1));
    evdev_close(&o.e);
}

/** Keep each replaced device open until no replacement slot remains, on
 * the auxiliary port or its pass-through guest. */
static void exhaustion_rows(int pass_through)
{
    struct opened held[EVDEV_MAX];
    struct evdev *old;
    struct event event;
    static const uint8_t motion[4] = {0x08, 7, 0, 0};
    int port = pass_through ? EVDEV_SERIO_PASS_THROUGH : 1;
    int i, n = 0, before, rc;
    const char *next;

    model_reset(KIND_MOUSE, 0);
    synaptics_answer = pt_present = pass_through;
    aux_mouse.wheel = pt_mouse.wheel = ID_IMPS;
    CHECK("the exhaustion row starts its device", init_mouse() == 0);
    for (i = 0; i < EVDEV_MAX; i++) {
        rc = open_phys(&held[i], pass_through ? GUEST_PHYS : "isa0060/serio1/input0");
        CHECK("the current device opens before replacement", rc == 0);
        if (rc < 0) {
            break;
        }
        n++;
        old = pass_through ? guest.dev : mouse_dev;
        before = pass_through ? guest.protocol : protocol;
        next = before == PROTO_PS2 ? "imps" : "bare";
        rc = store(port, "protocol", next);
        if (i + 1 < EVDEV_MAX) {
            CHECK("a replacement with a free slot succeeds", rc == 4);
            CHECK("the old descriptor reports removal",
                  evdev_read(&held[i].e, (char *)&event, sizeof(event)) == -ENODEV);
        } else {
            CHECK("a full table refuses the replacement", rc == -ENOMEM);
            CHECK("the working device and protocol survive the refusal",
                  old == (pass_through ? guest.dev : mouse_dev) &&
                      before == (pass_through ? guest.protocol : protocol));
            if (pass_through) {
                ps2mouse_guest_packet(motion);
            } else {
                irq_byte(motion[0]);
                irq_byte(motion[1]);
                irq_byte(motion[2]);
                if (packet_size == 4) {
                    irq_byte(motion[3]);
                }
            }
            CHECK("the last descriptor still reads motion", has_event(&held[i], EV_REL, REL_X, 7));
            CHECK("reporting and the files remain", aux_mouse.reporting &&
                                                        (!pass_through || pt_mouse.reporting) &&
                                                        file_of(port, "protocol") != NULL);
        }
    }
    for (i = 0; i < n; i++) {
        evdev_close(&held[i].e);
    }
    CHECK("closing the old descriptors permits another switch",
          store(port, "protocol",
                (pass_through ? guest.protocol : protocol) == PROTO_PS2 ? "imps" : "bare") == 4);
}

/** A full table must preserve a touchpad too; one spare must suffice for
 * switching away and back after the old pad's descriptor closes. */
static void pad_exhaustion_rows(void)
{
    struct evdev *reserved[EVDEV_MAX], *old;
    struct opened o;
    int n = 0, i;

    model_reset(KIND_PAD, V4_CLICKPAD);
    CHECK("a pad starts before exhausting the table", init_mouse() == 0 && etd_pad);
    CHECK("the pad's descriptor opens", open_phys(&o, "isa0060/serio1/input0") == 0);
    while (n < EVDEV_MAX && (reserved[n] = evdev_reserve()) != NULL) {
        n++;
    }
    old = etd_pad;
    CHECK("a pad with no replacement slot remains registered",
          store(1, "protocol", "bare") == -ENOMEM && etd_pad == old && protocol == PROTO_ELANTECH &&
              file_of(1, "reg_07") != NULL && reporting);
    CHECK("the reservations filled every spare slot", n == EVDEV_MAX - 1);
    if (n) {
        evdev_unregister(reserved[--n]);
    }
    CHECK("one spare suffices for switching a pad to a mouse",
          store(1, "protocol", "bare") == 4 && mouse_dev && !etd_pad);
    CHECK("switching back without a spare keeps the mouse",
          store(1, "protocol", "elantech") == -ENOMEM && protocol == PROTO_PS2 && mouse_dev);
    evdev_close(&o.e);
    CHECK("closing the old pad permits its protocol again",
          store(1, "protocol", "elantech") == 8 && etd_pad && protocol == PROTO_ELANTECH);
    for (i = 0; i < n; i++) {
        evdev_unregister(reserved[i]);
    }
}

/** The files of a Synaptics pad's pass-through port, serio2, and the
 * changes through them, which disable the pad and the guest around them. */
static void guest_rows(void)
{
    static const uint8_t pad_frame[] = {0xF5, 0xF4};
    static const uint8_t guest_rate[] = {0xF5, 0xF3, 80, 0xF4};

    model_reset(KIND_MOUSE, 0);
    synaptics_answer = 1;
    pt_present = 1;
    pt_mouse.wheel = ID_IMPS;
    CHECK("a pad with a mouse behind its pass-through port is brought up",
          init_mouse() == 0 && touchpad == TOUCHPAD_SYNAPTICS && guest.dev != NULL);
    CHECK("serio1's protocol reads SynPS/2", reads(1, "protocol", "SynPS/2\n"));
    CHECK("serio2 has psmouse's five files", port_driver[EVDEV_SERIO_PASS_THROUGH] == guest_files &&
                                                 file_of(EVDEV_SERIO_PASS_THROUGH, "protocol") &&
                                                 file_of(EVDEV_SERIO_PASS_THROUGH, "resync_time"));
    CHECK("its protocol reads ImPS/2", reads(EVDEV_SERIO_PASS_THROUGH, "protocol", "ImPS/2\n"));
    CHECK("its rate 100, resolution 200, resetafter 5 and resync_time 0",
          reads(EVDEV_SERIO_PASS_THROUGH, "rate", "100\n") &&
              reads(EVDEV_SERIO_PASS_THROUGH, "resolution", "200\n") &&
              reads(EVDEV_SERIO_PASS_THROUGH, "resetafter", "5\n") &&
              reads(EVDEV_SERIO_PASS_THROUGH, "resync_time", "0\n"));
    n_took = 0;
    pt_mouse.n_took = 0;
    CHECK("a rate written to the guest is taken",
          store(EVDEV_SERIO_PASS_THROUGH, "rate", "80") == 2);
    CHECK("with the pad disabled around it", took_bytes(pad_frame, 2));
    CHECK("and the guest", pt_mouse.n_took == 4 && pt_mouse.took[0] == guest_rate[0] &&
                               pt_mouse.took[1] == guest_rate[1] &&
                               pt_mouse.took[2] == guest_rate[2] &&
                               pt_mouse.took[3] == guest_rate[3]);
    CHECK("which reads 80", reads(EVDEV_SERIO_PASS_THROUGH, "rate", "80\n") &&
                                pt_mouse.rate == 80 && pt_mouse.reporting);
    CHECK("the pad's rate goes through Synaptics' set_rate",
          store(1, "rate", "60") == 2 && synaptics_rate == 60 && reads(1, "rate", "40\n"));
}

/** The guest's protocol switches, refusals and removal with its parent. */
static void guest_switch_rows(void)
{
    CHECK("the guest refuses an embedded NUL and its suffix",
          store_bytes(EVDEV_SERIO_PASS_THROUGH, "protocol", "bare\0junk", 9) == -EINVAL &&
              guest.protocol == PROTO_IMPS);
    CHECK("the guest does not take synaptics",
          store(EVDEV_SERIO_PASS_THROUGH, "protocol", "synaptics") == -EINVAL);
    CHECK("bare switches it to PS/2", store(EVDEV_SERIO_PASS_THROUGH, "protocol", "bare") == 4 &&
                                          guest.protocol == PROTO_PS2 &&
                                          reads(EVDEV_SERIO_PASS_THROUGH, "protocol", "PS/2\n"));
    CHECK("on a new device, keeping the guest's name",
          count_named("PS/2 Generic Wheel Mouse") == 1 &&
              count_named("ImPS/2 Generic Wheel Mouse") == 0);
    CHECK("imps finds the wheel again", store(EVDEV_SERIO_PASS_THROUGH, "protocol", "imps") == 4 &&
                                            guest.protocol == PROTO_IMPS && pt_mouse.reporting &&
                                            aux_mouse.reporting);
    guest.active = 0;
    CHECK("a write while the guest waits for its reconnect is -ENODEV",
          store(EVDEV_SERIO_PASS_THROUGH, "rate", "40") == -ENODEV);
    guest.active = 1;
    CHECK("switching the pad to bare removes its child port's callbacks",
          store(1, "protocol", "bare") == 4 && !guest.dev &&
              !file_of(EVDEV_SERIO_PASS_THROUGH, "protocol") &&
              !port_driver[EVDEV_SERIO_PASS_THROUGH]);
    CHECK("switching back recreates the guest's files",
          store(1, "protocol", "synaptics") == 9 && guest.dev &&
              file_of(EVDEV_SERIO_PASS_THROUGH, "protocol") &&
              port_driver[EVDEV_SERIO_PASS_THROUGH] == guest_files);
}

/** Whether reg_10 reads 0x5b. */
static int reading_10(void)
{
    return reads(1, "reg_10", "0x5b\n");
}

/* What an Elantech pad's register file reads when the read fails. */
#define READ_FAILED "0xffffffff\n"

/** The Elantech pad's files after its bring-up: the thirteen of
 * elantech.c, registers read from the pad, and the settings. */
static void elantech_rows(void)
{
    static const char *const names[13] = {"reg_07", "reg_10",      "reg_11",     "reg_20", "reg_21",
                                          "reg_22", "reg_23",      "reg_24",     "reg_25", "reg_26",
                                          "debug",  "paritycheck", "crc_enabled"};
    int i, all = 1;

    model_reset(KIND_PAD, V4_CLICKPAD);
    CHECK("a version 4 pad is brought up", init_mouse() == 0 && touchpad == TOUCHPAD_ELANTECH);
    for (i = 0; i < 13; i++) {
        if (!file_of(1, names[i]) || file_of(1, names[i])->attr.mode != 0644u) {
            all = 0;
        }
    }
    CHECK("serio1 has elantech.c's thirteen files beside psmouse's", all && n_files == 18);
    CHECK("protocol reads ETPS/2", reads(1, "protocol", "ETPS/2\n"));
    CHECK("absolute mode is in register 0x07", regs[0x07] == 0x01);
    CHECK("whose read gives its second byte, as elantech.c takes version 4's",
          (reg07_hi = 0x3c, reads(1, "reg_07", "0x3c\n")) && etd_regs[REG_07] == 0x3c);
    CHECK("reg_10 reads the pad's register", (regs[0x10] = 0x5a, reads(1, "reg_10", "0x5a\n")));
    CHECK("a read runs with the pad disabled and leaves it reporting",
          took[n_took - 1] == 0xF4 && reporting && state == STATE_ACTIVE);
    CHECK("debug reads 1, a pad past version 1", reads(1, "debug", "0x01\n"));
    CHECK("paritycheck reads 1", reads(1, "paritycheck", "0x01\n"));
    CHECK("crc_enabled reads 0 for firmware 0x581f01", reads(1, "crc_enabled", "0x00\n"));
}

/** Writes of the pad's files: a register written to the pad and read
 * back, kstrtou8's refusals, a write the pad refuses, the settings, and
 * the port's files on the pad; and each while the port waits for its
 * reconnect. */
static void elantech_write_rows(void)
{
    static const uint8_t write07[] = {0xF5, 0xF8, 0x00, 0xF8, 0x07, 0xF8,
                                      0x00, 0xF8, 0x05, 0xE6, 0xF4};

    n_took = 0;
    CHECK("a write of 5 to reg_07 is taken whole", store(1, "reg_07", "5\n") == 2);
    CHECK("as version 4's register write, with the pad disabled around it",
          took_bytes(write07, 11));
    CHECK("the pad holds it", regs[0x07] == 0x05 && etd_regs[REG_07] == 0x05);
    CHECK("reg_10 reads back what it took",
          store(1, "reg_10", "5b") == 2 && regs[0x10] == 0x5b && reading_10());
    CHECK("and reports", reporting && state == STATE_ACTIVE);
    CHECK("hex with 0x is taken",
          store(1, "reg_26", "0x1F") == 4 && regs[0x26] == 0x1f && reads(1, "reg_26", "0x1f\n"));
    CHECK("a byte past 0xff is -ERANGE", store(1, "reg_20", "1ff") == -ERANGE);
    CHECK("text that is no hex is -EINVAL", store(1, "reg_20", "zz") == -EINVAL);
    CHECK("and neither writes the pad", regs[0x20] == 0 && reporting);
    refuse_reg = 0x21;
    CHECK("a write the pad refuses is still taken whole, as elantech.c answers it",
          store(1, "reg_21", "33") == 2 && regs[0x21] == 0);
    refuse_reg = -1;
    CHECK("crc_enabled takes 1 and changes no register", store(1, "crc_enabled", "1") == 1 &&
                                                             reads(1, "crc_enabled", "0x01\n") &&
                                                             etd_info.crc_enabled == 1);
    CHECK("paritycheck takes 0", store(1, "paritycheck", "0") == 1 && etd_info.paritycheck == 0);
    CHECK("debug takes 2", store(1, "debug", "2\n") == 2 && reads(1, "debug", "0x02\n"));
    CHECK("the pad's rate is set", store(1, "rate", "80") == 2 && reads(1, "rate", "80\n"));
    CHECK("its resolution",
          store(1, "resolution", "100") == 3 && dev_res == 2 && reads(1, "resolution", "100\n"));
    CHECK("and it reports after each", reporting && state == STATE_ACTIVE);

    state = STATE_IGNORE;
    CHECK("a register read while the port waits for its reconnect fails",
          reads(1, "reg_07", READ_FAILED));
    CHECK("its write is -ENODEV", store(1, "reg_07", "1") == -ENODEV);
    CHECK("a setting still reads", reads(1, "debug", "0x02\n"));
    state = STATE_ACTIVE;
}

/** The switch away from the pad's protocol and back, with its files and
 * its device. */
static void elantech_switch_rows(void)
{
    CHECK("bare switches the pad away from its protocol",
          store(1, "protocol", "bare") == 4 && protocol == PROTO_PS2 && touchpad == 0);
    CHECK("its device named as psmouse names it, keeping the pad's vendor",
          count_named("PS/2 Elantech Touchpad") == 1 &&
              count_named("ETPS/2 Elantech Touchpad") == 0);
    CHECK("its files leave serio1", file_of(1, "reg_07") == NULL && n_files == 5);
    CHECK("elantech sets the pad up again", store(1, "protocol", "elantech") == 8 &&
                                                touchpad == TOUCHPAD_ELANTECH &&
                                                protocol == PROTO_ELANTECH);
    CHECK("with its files and its device", file_of(1, "reg_07") != NULL && n_files == 18 &&
                                               count_named("ETPS/2 Elantech Touchpad") == 1 &&
                                               count_named("PS/2 Elantech Touchpad") == 0);
    CHECK("its settings Linux's again", reads(1, "paritycheck", "0x01\n") && regs[0x07] == 0x01);
}

/* An Elantech callback that has entered before its files are removed. */
static struct file waiting_file;
static int waiting_kind;
static int32_t waiting_result;

/** Enter the saved callback, whose command wait parks this call. */
static void waiting_access(void)
{
    char buf[32];

    if (waiting_kind == 0) {
        waiting_result =
            waiting_file.attr.show(waiting_file.obj, waiting_file.arg, buf, sizeof(buf));
    } else {
        waiting_result = waiting_file.attr.store(waiting_file.obj, waiting_file.arg, "5b", 2);
    }
}

/** Resume a read, register write and setting write after a switch removed
 * their protocol; in the last case it has also bound a new Elantech pad. */
static void elantech_wait_rows(void)
{
    static const uint8_t resumed[] = {0xF5, 0xF4};
    int kind, done;
    uint8_t reg, debug;

    for (kind = 0; kind < 3; kind++) {
        model_reset(KIND_PAD, V4_CLICKPAD);
        CHECK("the concurrent row starts an Elantech pad", init_mouse() == 0 && etd_pad);
        waiting_file = *file_of(1, kind == 2 ? "debug" : "reg_10");
        waiting_kind = kind;
        waiting_result = 0;
        CHECK("a protocol change takes the command lock", ps2mouse_change_begin() == 0);
        done = park_run(waiting_access);
        CHECK("the attribute callback waits for the switch", done == 0 && cmd_busy);
        CHECK("the switch removes Elantech", aux_set_protocol("bare", 4) == 0);
        if (kind == 2) {
            CHECK("a new Elantech protocol binds before the waiter wakes",
                  aux_set_protocol("elantech", 8) == 0 && etd_pad);
        }
        ps2mouse_change_end(0);
        reg = regs[0x10];
        debug = etd_info.debug;
        n_took = 0;
        if (!done) {
            CHECK("the waiting callback returns after the switch", park_resume() == 1);
        }
        CHECK("the old callback fails with ENODEV", waiting_result == -ENODEV);
        CHECK("it sends no Elantech command to the new protocol",
              took_bytes(resumed, sizeof(resumed)) && regs[0x10] == reg && etd_info.debug == debug);
        CHECK("the port is reporting with its command lock free",
              !cmd_busy && state == STATE_ACTIVE && reporting);
    }
}

/** The rate's restore of register 0x07 on firmware 0x381f17, and version
 * 1's mode bits forced on a write. */
static void elantech_other_rows(void)
{
    static const uint8_t rate80[] = {0xF5, 0xF3, 80,   0xF8, 0x00, 0xF8, 0x07,
                                     0xF8, 0x00, 0xF8, 0x01, 0xE6, 0xF4};

    model_reset(KIND_PAD, V4_TP500);
    CHECK("a pad of firmware 0x381f17 is brought up",
          init_mouse() == 0 && touchpad == TOUCHPAD_ELANTECH);
    n_took = 0;
    CHECK("a rate written", store(1, "rate", "80") == 2);
    CHECK("writes register 0x07 again after it, as elantech.c's set_rate does",
          took_bytes(rate80, 13));

    model_reset(KIND_PAD, V1_ROCKER);
    CHECK("a version 1 pad is brought up", init_mouse() == 0 && touchpad == TOUCHPAD_ELANTECH);
    CHECK("its debug reads 0", reads(1, "debug", "0x00\n"));
    CHECK("reg_10 keeps absolute mode on a write",
          store(1, "reg_10", "0") == 1 && regs[0x10] == 0x04 && reads(1, "reg_10", "0x04\n"));
    CHECK("reg_11 keeps four-byte packets on a write",
          store(1, "reg_11", "80") == 2 && regs[0x11] == 0x82 && reads(1, "reg_11", "0x82\n"));
    CHECK("another register takes the byte as it is",
          store(1, "reg_20", "80") == 2 && regs[0x20] == 0x80);
    CHECK("the pad reports after each", reporting && state == STATE_ACTIVE);
}

int main(void)
{
    plain_rows();
    rate_rows();
    resolution_rows();
    resetafter_rows();
    resync_time_rows();
    resync_switch_rows();
    protocol_rows();
    switch_rows();
    detect_rows();
    guest_rows();
    guest_switch_rows();
    exhaustion_rows(0);
    exhaustion_rows(1);
    pad_exhaustion_rows();
    elantech_rows();
    elantech_write_rows();
    elantech_switch_rows();
    elantech_other_rows();
    elantech_wait_rows();
    model_reset(KIND_MOUSE, 0);
    if (fails == 0) {
        printf("psmouse-attr-test: pass\n");
    }
    return fails ? 1 : 0;
}
