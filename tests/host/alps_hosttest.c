/**
 * The ALPS touchpad's identification, set-up and packets (sys/arch/x86/
 * alps.c), through the PS/2 mouse's bring-up and IRQ 12 handler
 * (ps2mouse.c), against a model of the 8042 and of an ALPS pad of each
 * protocol version behind it, with the event devices read as a program
 * reads them (evdev.c) and the mouse device's packets (mouse.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The driver's files are included whole, so that their static state is
 * reachable; the Synaptics probe, which runs first and which
 * synaptics_hosttest.c tests, is a stub that finds no Synaptics pad unless
 * a row says it answered, and the Elantech probe, which runs after ALPS's
 * and which elantech_hosttest.c tests, a stub that counts its calls and
 * finds no pad. The SMBIOS reader reads a table a row gives it, whose
 * System Information names the machine.
 *
 * The model pad answers what Linux 6.12's alps.c sends, as
 * Documentation/input/devices/alps.rst describes the hardware: the E6, E7
 * and EC reports, three of a scaling after a resolution of 0 and a status
 * request, give the row's E6 rate, E7 signature and EC firmware, and the
 * EC report enters command mode, which stream mode (0xEA) leaves. In
 * command mode a register address is the version's address command (0xEC,
 * or 0xF5 on version 4) and four nibbles, each a PS/2 command of the
 * version's table, a status request reads the register, and two nibbles
 * write it. Version 6's monitor mode, entered by EC E9 F5 F5 E7 E6 E7 E9,
 * takes three words of three nibbles of its own table between an enable
 * and a disable. A version 8 pad answers F0 F0 E9 and EA EA E9 with its
 * one-time programmable bytes. Commands sent while a version 2 pad's pass-
 * through is on (E7 E7 E7 F5, off by E6 E6 E6 F5), or a version 3 pad's
 * pass-through register bit, are the stick's and are logged.
 *
 * The rows check the probe of each row of Linux's `alps_model_data` and of
 * each version the EC report tells apart, the commands and the registers
 * each version's set-up leaves, the event devices' names, numbers and
 * capabilities in /proc/bus/input/devices' form, and the events, slots and
 * mouse packets each version's packets give, all as Linux 6.12's alps.c,
 * psmouse and input-mt compute them for the same bytes. Each packet's
 * bytes are built from alps.rst's tables; where alps.rst and Linux's code
 * differ, the code's layout is used (dev-log/76-display/upstream.md).
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2mouse.c"
#include "arch/x86/alps.c"

#include "fs/fd.h"
#include "lib/kerrno.h"
#include "proc/wait_queue.h"

int printf(const char *format, ...);
int sprintf(char *s, const char *format, ...);
int strcmp(const char *a, const char *b);
char *strstr(const char *haystack, const char *needle);

static int fails;

#define CHECK(what, cond)                          \
    do {                                           \
        if (!(cond)) {                             \
            printf("alps-test: fail: %s\n", what); \
            fails++;                               \
        }                                          \
    } while (0)

/* The status of a byte the mouse sent, and the configuration byte the
 * firmware leaves. */
#define MOUSE_BYTE      (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)
#define FIRMWARE_CONFIG 0x65

/* The ticks of the kernel's clock, which a row moves on. */
volatile uint64_t kernel_ticks;

/* The model 8042's output buffer, a queue of bytes the device sent. */
#define MODEL_BYTES 64
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;

/* The controller's configuration byte, and what the next write to port
 * 0x60 is for: 0, or the command that named it. */
static uint8_t ctl_config;
static uint8_t ctl_next;

/* An ALPS pad of the model: its E6 rate, E7 signature and EC firmware, its
 * command mode's nibble table (3, 4, or 0 for none), and a version 8 pad's
 * OTP bytes, the answers of EA EA E9 and F0 F0 E9. */
struct pad_model {
    uint8_t e6;
    uint8_t e7[3];
    uint8_t ec[3];
    int table;
    uint8_t otp0[3];
    uint8_t otp1[3];
};

/* What the auxiliary device is. */
#define KIND_ALPS  1
#define KIND_PLAIN 2 /* a PS/2 mouse of three buttons and no wheel */

/* The device: its kind and model; its registers; the commands it took last,
 * newest last; the command whose argument comes next; its resolution,
 * rate, 2:1 scaling and whether it reports; command mode, the address and
 * its nibbles so far (-1 for none), the value's nibbles; monitor mode, its
 * nibbles (-1 before the enable that starts them) and the last word it
 * wrote; a version 2 pad's pass-through and the register of a version 3
 * pad's (-1 none); whether a version 1 or 2 pad took its absolute knock;
 * the argument it leaves unanswered (-1 none); whether it answers the
 * TrackPoint's identity as the CS19; and every byte it and its stick took. */
static int dev_kind;
static struct pad_model model;
static uint8_t regs[65536];
#define HIST 8
static uint8_t hist[HIST];
static uint8_t arg_for;
static uint8_t dev_res;
static uint8_t dev_rate;
static int scale21;
static int reporting;
static int cmd_mode;
static int addr;
static int addr_n;
static int val;
static int val_n;
static int mon_mode;
static int mon_n;
static int mon_nib[9];
static int mon_word0;
static int mon_addr;
static int mon_value;
static int mon_writes;
static int v2pass;
static int pass_reg;
static int v12_abs;
static int refuse_arg;
static int cs19;
#define TOOK_MAX 1024
static uint8_t took[TOOK_MAX];
static int n_took;
static uint8_t stick_took[64];
static int n_stick_took;

/* What the Synaptics probe returns, the Elantech probe's calls, the SMBIOS
 * table, and the warnings logged, the last of them, and the interrupts'
 * state. */
static int synaptics_answer;
static int elantech_asked;
static uint8_t smbios_table[96];
static int warnings;
static const char *last_warning;
static const char *warning_log[16];
static int irq_on = 1;
static isr_handler_t registered_handler;

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("alps-test: fail: the model's output buffer overflowed\n");
        fails++;
        return;
    }
    model_data[model_head + model_count] = data;
    model_count++;
}

/** Put the three bytes of `b` in the output buffer. */
static void put3(const uint8_t *b)
{
    model_put(b[0]);
    model_put(b[1]);
    model_put(b[2]);
}

/** Put a mouse's status: mode bits, resolution and rate. */
static void put_status(void)
{
    model_put((uint8_t)((reporting ? 0x20 : 0) | (scale21 ? 0x10 : 0)));
    model_put(dev_res);
    model_put(dev_rate);
}

/** Whether the last `n` commands taken are `cmds`, the oldest first. */
static int hist_is(const uint8_t *cmds, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (hist[HIST - n + i] != cmds[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether the last three commands are each `c`. */
static int three_of(uint8_t c)
{
    return hist[HIST - 1] == c && hist[HIST - 2] == c && hist[HIST - 3] == c;
}

/** Add the command `c` to the history. */
static void hist_push(uint8_t c)
{
    int i;

    for (i = 0; i < HIST - 1; i++) {
        hist[i] = hist[i + 1];
    }
    hist[HIST - 1] = c;
}

/** The nibble of the command `cmd` with the argument `arg` in the table
 * `table` (3, 4 or 6), as Linux's alps_v3_nibble_commands,
 * alps_v4_nibble_commands and alps_v6_nibble_commands list them, or -1. */
static int nibble_of(int table, uint8_t cmd, uint8_t arg)
{
    static const uint8_t rates[7] = {0x0a, 0x14, 0x28, 0x3c, 0x50, 0x64, 0xc8};
    int i;

    switch (cmd) {
    case 0xF0:
        return table == 3 ? 0 : -1;
    case 0xF4:
        return table == 4 || table == 6 ? 0 : -1;
    case 0xF6:
        return table == 6 ? -1 : 1;
    case 0xE7:
        return table == 6 ? 14 : 2;
    case 0xF3:
        for (i = 0; i < 7; i++) {
            if (rates[i] == arg) {
                return table == 6 ? 1 + i : 3 + i;
            }
        }
        return -1;
    case 0xF2:
        return table == 6 ? 8 : 10;
    case 0xE9:
        return table == 6 ? 9 : -1;
    case 0xE8:
        return arg > 3 ? -1 : (table == 6 ? 10 : 11) + arg;
    case 0xE6:
        return 15;
    default:
        return -1;
    }
}

/** Take the nibble `n` in command mode: the address's, then the value's. */
static void cmd_nibble(int n)
{
    if (n < 0 || addr_n < 0) {
        return;
    }
    if (addr_n < 4) {
        addr = (addr << 4 | n) & 0xffff;
        addr_n++;
        val_n = 0;
        return;
    }
    val = (val << 4 | n) & 0xff;
    if (++val_n == 2) {
        regs[addr] = (uint8_t)val;
        val_n = 0;
    }
}

/** Take the nibble `n` in monitor mode. */
static void mon_nibble(int n)
{
    if (n >= 0 && mon_n >= 0 && mon_n < 9) {
        mon_nib[mon_n++] = n;
    }
}

/** Whether a command now reaches the stick through the pad. */
static int stick_pass(void)
{
    return !cmd_mode && (v2pass || (pass_reg >= 0 && (regs[pass_reg] & 1)));
}

/** Take the argument `a` of the command `cmd`, after its ACK. */
static void dev_arg(uint8_t cmd, uint8_t a)
{
    if (cmd_mode) {
        cmd_nibble(nibble_of(model.table, cmd, a));
    } else if (mon_mode) {
        mon_nibble(nibble_of(6, cmd, a));
    } else if (cmd == 0xE8) {
        dev_res = a;
    } else {
        dev_rate = a;
    }
    hist_push(cmd);
}

/** Take the command `b` in command mode, after its ACK. */
static void dev_cmd_mode(uint8_t b)
{
    uint8_t reg[3];

    if (b == 0xEA) {
        cmd_mode = 0;
    } else if (b == (model.table == 4 ? 0xF5 : 0xEC)) {
        addr = 0;
        addr_n = 0;
    } else if (b == 0xE9) {
        reg[0] = (uint8_t)(addr >> 8);
        reg[1] = (uint8_t)addr;
        reg[2] = regs[addr];
        put3(reg);
        val_n = 0;
    } else {
        if (b == 0xF2) {
            model_put(0x00);
        }
        cmd_nibble(nibble_of(model.table, b, 0));
    }
}

/** Take the command `b` in monitor mode, after its ACK. */
static void dev_mon_mode(uint8_t b)
{
    if (b == 0xEC) {
        mon_mode = 0;
    } else if (b == 0xF5) {
        if (mon_n == 9) {
            mon_word0 = mon_nib[0] | mon_nib[1] << 4 | mon_nib[2] << 8;
            mon_addr = mon_nib[3] | mon_nib[4] << 4 | mon_nib[5] << 8;
            mon_value = mon_nib[6] | mon_nib[7] << 4 | mon_nib[8] << 8;
            mon_writes++;
        }
        mon_n = -1;
    } else if (b == 0xF4 && mon_n < 0) {
        mon_n = 0;
    } else {
        if (b == 0xF2) {
            model_put(0x00);
        } else if (b == 0xE9) {
            put_status();
        }
        mon_nibble(nibble_of(6, b, 0));
    }
}

/** Answer a status request outside command and monitor mode. */
static void dev_status(void)
{
    static const uint8_t monitor[7] = {0xEC, 0xE9, 0xF5, 0xF5, 0xE7, 0xE6, 0xE7};
    static const uint8_t otp1[2] = {0xF0, 0xF0};
    static const uint8_t otp0[2] = {0xEA, 0xEA};
    uint8_t e6[3];

    if (dev_kind != KIND_ALPS) {
        put_status();
    } else if (three_of(0xE6)) {
        e6[0] = e6[1] = 0;
        e6[2] = model.e6;
        put3(e6);
    } else if (three_of(0xE7)) {
        put3(model.e7);
    } else if (three_of(0xEC)) {
        put3(model.ec);
        cmd_mode = 1;
        addr_n = -1;
    } else if (hist_is(monitor, 7)) {
        put_status();
        mon_mode = 1;
        mon_n = -1;
    } else if (hist_is(otp1, 2)) {
        put3(model.otp1);
    } else if (hist_is(otp0, 2)) {
        put3(model.otp0);
    } else {
        put_status();
    }
}

/** Take the command `b`, after its ACK, outside command and monitor mode. */
static void dev_command(uint8_t b)
{
    static const uint8_t knock[4] = {0xF5, 0xF5, 0xF5, 0xF5};

    switch (b) {
    case 0xE9:
        dev_status();
        break;
    case 0xE6:
        scale21 = 0;
        break;
    case 0xE7:
        scale21 = 1;
        break;
    case 0xF2:
        model_put(0x00);
        break;
    case 0xF4:
        reporting = 1;
        if (hist_is(knock, 4)) {
            v12_abs = 1;
        }
        break;
    case 0xF5:
        reporting = 0;
        if (three_of(0xE7)) {
            v2pass = 1;
        } else if (three_of(0xE6)) {
            v2pass = 0;
        }
        break;
    case 0xF6:
        reporting = 0;
        dev_res = 2;
        dev_rate = 100;
        scale21 = 0;
        break;
    default:
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
    if (stick_pass() && n_stick_took < (int)sizeof(stick_took)) {
        stick_took[n_stick_took++] = b;
    }
    if (dev_kind == KIND_ALPS && refuse_arg >= 0 && b == refuse_arg) {
        arg_for = 0;
        return; /* left unanswered, and every try of it again */
    }
    if (arg_for) {
        cmd = arg_for;
        arg_for = 0;
        model_put(0xFA);
        dev_arg(cmd, b);
        return;
    }
    if (dev_kind == KIND_ALPS && b == 0xE1 && !cmd_mode && !mon_mode) {
        if (cs19) {
            model_put(0xFA);
            model_put(0x02);
            model_put(0x20);
        } else {
            model_put(0xFC); /* no TrackPoint here */
        }
        hist_push(b);
        return;
    }
    model_put(0xFA);
    if (b == 0xFF) {
        model_put(0xAA);
        model_put(0x00);
        cmd_mode = mon_mode = v2pass = 0;
        reporting = scale21 = 0;
        dev_res = 2;
        dev_rate = 100;
        k_memset(hist, 0, sizeof(hist));
        return;
    }
    if (b == 0xE8 || b == 0xF3) {
        arg_for = b;
        return;
    }
    if (cmd_mode) {
        dev_cmd_mode(b);
    } else if (mon_mode) {
        dev_mon_mode(b);
    } else {
        dev_command(b);
    }
    hist_push(b);
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

void outb(uint16_t port, uint8_t value)
{
    uint8_t next = ctl_next;

    if (port == PS2_CMD_PORT) {
        ctl_next = 0;
        if (value == PS2_CMD_ENABLE_MOUSE) {
            ctl_config &= ~0x20;
        } else if (value == PS2_CMD_READ_CONFIG) {
            model_put(ctl_config);
        } else if (value == PS2_CMD_WRITE_CONFIG || value == PS2_CMD_WRITE_MOUSE) {
            ctl_next = value;
        }
    } else if (port == PS2_DATA_PORT) {
        ctl_next = 0;
        if (next == PS2_CMD_WRITE_CONFIG) {
            ctl_config = value;
        } else if (next == PS2_CMD_WRITE_MOUSE) {
            dev_take(value);
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

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        if (warnings < 16) {
            warning_log[warnings] = format;
        }
        warnings++;
        last_warning = format;
    }
}

/** Whether a warning logged since the last reset holds `text`. */
static int warned(const char *text)
{
    int i;

    for (i = 0; i < warnings && i < 16; i++) {
        if (strstr(warning_log[i], text) != 0) {
            return 1;
        }
    }
    return 0;
}

/** The Synaptics probe, which runs before ALPS's: the row's answer. */
int synaptics_init(void)
{
    return synaptics_answer;
}

/** No Synaptics pad's bytes come. */
int synaptics_byte(uint8_t data)
{
    (void)data;
    return SYNAPTICS_GOOD;
}

/* The rest of synaptics.h and trackpoint.h, which a port without a
 * Synaptics pad never calls; trackpoint_hosttest.c tests them. */
void synaptics_packet_restart(void)
{}

int synaptics_packet_start(void)
{
    return 1;
}

int synaptics_has_pass_through(void)
{
    return 0;
}

/** A pass-through port that never answers. */
static int no_send(uint8_t cmd)
{
    (void)cmd;
    return -1;
}

/** Nothing comes from that port. */
static int no_receive(void)
{
    return -1;
}

const struct ps2io synaptics_pt = {no_send, no_receive};

void synaptics_pt_activate(int packet_size)
{
    (void)packet_size;
}

int synaptics_reconnect(void)
{
    return -1;
}

void synaptics_disconnect(void)
{}

/* The kind the stub's TrackPoint answered, 0 for none. */
static uint8_t tp_kind;

/** The TrackPoint's read-ID alone, which the CS19 answers as ALPS's
 * stick; trackpoint_hosttest.c tests the rest of the set-up. */
int trackpoint_detect(const struct ps2io *io)
{
    uint8_t id[2] = {0, 0};

    tp_kind = 0;
    if (ps2io_command(io, 0xE1, 0, 0, id, 2) < 0 || id[0] < 1 || id[0] > 6) {
        return -1;
    }
    tp_kind = id[0];
    return 0;
}

int trackpoint_identify(const struct ps2io *io)
{
    (void)io;
    return -1;
}

const char *trackpoint_vendor(void)
{
    return tp_kind == 2 ? "ALPS" : "";
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

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    (void)port;
    (void)add;
}

/** The 10 ms a pass-through probe waits, which no ALPS row reaches. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** The Elantech probe, which runs when no ALPS pad answered: none here. */
int elantech_init(void)
{
    elantech_asked++;
    return 0;
}

/** No Elantech pad's bytes come, since `elantech_init` found none. */
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

/** No table is mapped: the SMBIOS reader reads the row's from memory. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return 0;
}

/** Give the SMBIOS reader a table whose System Information, type 1, names
 * the manufacturer `maker` and the product `product`, or, when `maker` is
 * NULL, a table of no structure but its end. */
static void set_system(const char *maker, const char *product)
{
    static const uint8_t end[6] = {127, 4, 0, 0, 0, 0};
    unsigned int n = 0, i;

    if (maker) {
        static const uint8_t header[8] = {1, 8, 0, 1, 1, 2, 0, 0};

        k_memcpy(smbios_table, header, sizeof(header));
        n = sizeof(header);
        for (i = 0; maker[i]; i++) {
            smbios_table[n++] = (uint8_t)maker[i];
        }
        smbios_table[n++] = 0;
        for (i = 0; product[i]; i++) {
            smbios_table[n++] = (uint8_t)product[i];
        }
        smbios_table[n++] = 0;
        smbios_table[n++] = 0;
    }
    k_memcpy(smbios_table + n, end, sizeof(end));
    smbios_set_table(smbios_table, n + (unsigned int)sizeof(end));
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

/* An open of an event device: its descriptor and description. */
struct node {
    fd_entry_t e;
    struct open_file ofd;
    int open;
};

static struct node n_pad, n_stick, n_bare;

/* The records the last `take` read. */
static struct record got[64];
static int n_got;

/** Close the open `n`, if it is open. */
static void close_node(struct node *n)
{
    if (n->open) {
        evdev_close(&n->e);
        n->open = 0;
    }
}

/** Open, nonblocking, the event device named `name`. Returns 1, or 0 when
 * no device has that name. */
static int open_named(struct node *n, const char *name)
{
    char text[64];
    int i;

    close_node(n);
    for (i = 0; i < 32; i++) {
        if (!evdev_present(i)) {
            continue;
        }
        k_memset(n, 0, sizeof(*n));
        n->ofd.status_flags = OFD_NONBLOCK;
        n->e.ofd = &n->ofd;
        if (evdev_open(&n->e, i) != 0) {
            continue;
        }
        if (evdev_ioctl(&n->e, EVIOCGNAME(64), (uintptr_t)text) > 0 && strcmp(text, name) == 0) {
            n->open = 1;
            return 1;
        }
        evdev_close(&n->e);
    }
    return 0;
}

/** Read every whole packet of `n` into `got`. */
static int32_t take(struct node *n)
{
    int32_t rc = n->open ? evdev_read(&n->e, (char __user *)got, sizeof(got)) : -EBADF;

    n_got = rc > 0 ? rc / (int32_t)sizeof(got[0]) : 0;
    return rc;
}

/* An event a row expects. */
struct event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define NEV(a) ((int)(sizeof(a) / sizeof((a)[0])))

/** Whether `got` holds the `n` events of `want` and nothing else. */
static int got_events(const struct event *want, int n)
{
    int i;

    if (n_got != n) {
        printf("alps-test: read %d records, not %d\n", n_got, n);
        for (i = 0; i < n_got; i++) {
            printf("alps-test:   %u %u %d\n", got[i].type, got[i].code, (int)got[i].value);
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (got[i].type != want[i].type || got[i].code != want[i].code ||
            got[i].value != want[i].value) {
            printf("alps-test: record %d is %u %u %d, not %u %u %d\n", i, got[i].type, got[i].code,
                   (int)got[i].value, want[i].type, want[i].code, (int)want[i].value);
            return 0;
        }
    }
    return 1;
}

/** Whether reading `n` gives the events of `want` alone. */
static int reads(struct node *n, const struct event *want, int count)
{
    take(n);
    return got_events(want, count);
}

/** Whether `n` is open and has nothing to read. */
static int reads_nothing(struct node *n)
{
    return take(n) == -EAGAIN;
}

/** Put the controller and a device of `kind` as the firmware leaves them,
 * with no fault, and take away what the last row registered. */
static void model_reset(int kind)
{
    elantech_asked = 0;
    close_node(&n_pad);
    close_node(&n_stick);
    close_node(&n_bare);
    evdev_unregister(bare);
    bare = 0;
    unregister_all();
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
    bare_failed = 0;
    rejected_stick = 0;
    sync_losses = 0;
    pktcnt = 0;
    out_of_sync = 0;
    flush_armed = 0;
    model_head = 0;
    model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_kind = kind;
    k_memset(&model, 0, sizeof(model));
    k_memset(regs, 0, sizeof(regs));
    k_memset(hist, 0, sizeof(hist));
    arg_for = 0;
    dev_res = 2;
    dev_rate = 100;
    scale21 = 0;
    reporting = 0;
    cmd_mode = 0;
    addr = 0;
    addr_n = -1;
    val = val_n = 0;
    mon_mode = 0;
    mon_n = -1;
    mon_word0 = mon_addr = mon_value = -1;
    mon_writes = 0;
    v2pass = 0;
    pass_reg = -1;
    v12_abs = 0;
    refuse_arg = -1;
    cs19 = 0;
    n_took = 0;
    n_stick_took = 0;
    synaptics_answer = 0;
    set_system(0, 0);
    warnings = 0;
    last_warning = 0;
    registered_handler = 0;
    kernel_ticks += MS_TO_TICKS(1000);
}

/** Make the device an ALPS pad of the reports `e7` and `ec`, an E6 rate of
 * 100, and the command table `table`. */
static void alps_model(const uint8_t *e7, const uint8_t *ec, int table)
{
    model_reset(KIND_ALPS);
    model.e6 = 100;
    k_memcpy(model.e7, e7, 3);
    k_memcpy(model.ec, ec, 3);
    model.table = table;
}

/* The device's bytes a row expects, built with `expect` and `expect_n`. */
static uint8_t want_took[TOOK_MAX];
static int n_want;

/** Expect the bytes `b`, `n` of them, sent to the device. */
static void expect_n(const uint8_t *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (n_want < TOOK_MAX) {
            want_took[n_want] = b[i];
        }
        n_want++;
    }
}

/** Expect what precedes alps_init, the reset and the defaults, and its
 * start: the defaults again, the trial identification, the CS19 check,
 * the reset and the identification kept. */
static void expect_probe(void)
{
    static const uint8_t start[3] = {0xFF, 0xF6, 0xF6};
    static const uint8_t ident[19] = {0xE8, 0x00, 0xE6, 0xE6, 0xE6, 0xE9, 0xE8, 0x00, 0xE7, 0xE7,
                                      0xE7, 0xE9, 0xE8, 0x00, 0xEC, 0xEC, 0xEC, 0xE9, 0xEA};
    static const uint8_t cs19_reset[2] = {0xE1, 0xFF};

    n_want = 0;
    expect_n(start, 3);
    expect_n(ident, 19);
    expect_n(cs19_reset, 2);
    expect_n(ident, 19);
}

/** Expect psmouse_initialize's rate, resolution and scaling, then the
 * enable of reporting. */
static void expect_initialize(void)
{
    static const uint8_t init[6] = {0xF3, 0x64, 0xE8, 0x03, 0xE6, 0xF4};

    expect_n(init, 6);
}

/** Whether the device took the bytes expected; prints the first that
 * differs. */
static int took_expected(void)
{
    int i;

    for (i = 0; i < n_want && i < n_took && i < TOOK_MAX; i++) {
        if (took[i] != want_took[i]) {
            printf("alps-test: byte %d to the device is 0x%02x, not 0x%02x\n", i, took[i],
                   want_took[i]);
            return 0;
        }
    }
    if (n_took != n_want) {
        printf("alps-test: the device took %d bytes, not %d\n", n_took, n_want);
        return 0;
    }
    return 1;
}

/** Whether the stick took `b`, `n` bytes, first. */
static int stick_took_first(const uint8_t *b, int n)
{
    int i;

    if (n_stick_took < n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (stick_took[i] != b[i]) {
            return 0;
        }
    }
    return 1;
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

/** Whether the set `what` (0 the properties, else EVIOCGBIT's type `what`
 * - 1) of `n` reads as `want` in /proc/bus/input/devices; prints it when
 * not. */
static int bits_are(struct node *n, int what, const char *want)
{
    uint8_t bits[96];
    char text[256];
    int longs = what == EV_KEY + 1 ? 12 : 1;

    k_memset(bits, 0, sizeof(bits));
    if (what == 0) {
        evdev_ioctl(&n->e, EVIOCGPROP(8), (uintptr_t)bits);
    } else {
        evdev_ioctl(&n->e, EVIOCGBIT(what - 1, (uint32_t)longs * 8), (uintptr_t)bits);
    }
    proc_bits(text, bits, longs);
    if (strcmp(text, want) != 0) {
        printf("alps-test: bits %d are %s, not %s\n", what, text, want);
        return 0;
    }
    return 1;
}

/* The sets `bits_are` reads. */
#define B_PROP 0
#define B_EV   (EV_SYN + 1)
#define B_KEY  (EV_KEY + 1)
#define B_REL  (EV_REL + 1)
#define B_ABS  (EV_ABS + 1)

/** Whether EVIOCGABS of `code` gives `min`, `max` and `res`, and no fuzz. */
static int abs_is(struct node *n, uint16_t code, int32_t min, int32_t max, int32_t res)
{
    struct input_absinfo a;

    if (evdev_ioctl(&n->e, EVIOCGABS(code), (uintptr_t)&a) != 0) {
        return 0;
    }
    if (a.minimum != min || a.maximum != max || a.fuzz != 0 || a.flat != 0 || a.resolution != res) {
        printf("alps-test: axis 0x%x is %d..%d res %d, not %d..%d res %d\n", code, a.minimum,
               a.maximum, a.resolution, min, max, res);
        return 0;
    }
    return 1;
}

/** Whether `n` is the device of `name` with Linux's place `phys` and
 * EVIOCGID numbers. */
static int ident_is(struct node *n, const char *phys, uint16_t product, uint16_t version)
{
    uint16_t id[4];
    char text[64];

    if (evdev_ioctl(&n->e, EVIOCGPHYS(64), (uintptr_t)text) <= 0 || strcmp(text, phys) != 0) {
        return 0;
    }
    evdev_ioctl(&n->e, EVIOCGID, (uintptr_t)id);
    return id[0] == 0x11 && id[1] == 2 && id[2] == product && id[3] == version;
}

/** Whether the stick's event device is Linux's for the version `version`,
 * with ABS_PRESSURE when `pressure`. */
static int stick_is(uint16_t version, int pressure)
{
    if (!open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick")) {
        return 0;
    }
    return ident_is(&n_stick, "isa0060/serio1/input1", PSMOUSE_ALPS, version) &&
           bits_are(&n_stick, B_PROP, "21") && bits_are(&n_stick, B_EV, pressure ? "f" : "7") &&
           bits_are(&n_stick, B_KEY, "70000 0 0 0 0") && bits_are(&n_stick, B_REL, "3") &&
           bits_are(&n_stick, B_ABS, pressure ? "1000000" : "0") &&
           (!pressure || abs_is(&n_stick, ABS_PRESSURE, 0, 127, 0));
}

/** Run IRQ 12 once with the device's byte `b` waiting. */
static void irq_byte(uint8_t b)
{
    model_head = 0;
    model_count = 0;
    model_put(b);
    mouse_irq_handler(0);
}

/** Send the `n` bytes of `p`, a packet, through IRQ 12. */
static void send(const uint8_t *p, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        irq_byte(p[i]);
    }
}

/** Read the mouse device's packets into `buf`; returns their count. */
static int mouse_packets(struct mouse_packet *buf, int max)
{
    return mouse_read_packets(buf, max);
}

/** Bring up the model device, as the boot does. Returns init_mouse's
 * answer. */
static int bring_up(void)
{
    int rc = init_mouse();

    mouse_clear_packets();
    return rc;
}

/*
 * The probe of every model of Linux's table: its protocol, packet form and
 * flags, the event devices' names, and the version 1 and 2 set-up.
 */

/** Every row of `alps_model_data`, identified by its E7 report alone. */
static void table_rows(void)
{
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    unsigned int i;
    int ok = 1, named = 1;

    for (i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        const struct model *m = &models[i];
        const char *name = m->protocol.flags & F_DUALPOINT ? "AlpsPS/2 ALPS DualPoint TouchPad"
                                                           : "AlpsPS/2 ALPS GlidePoint";

        alps_model(m->signature, ec, 0);
        if (bring_up() != 0 || alps != 1 || priv.proto_version != m->protocol.version ||
            priv.byte0 != m->protocol.byte0 || priv.mask0 != m->protocol.mask0 ||
            priv.flags != m->protocol.flags || !reporting) {
            printf("alps-test: model %02x %02x %02x is not set up as Linux sets it up\n",
                   m->signature[0], m->signature[1], m->signature[2]);
            ok = 0;
        }
        if (!open_named(&n_pad, name) || (stick != 0) != ((m->protocol.flags & F_DUALPOINT) != 0)) {
            named = 0;
        }
    }
    CHECK("each model of the table takes its row's protocol, packet form and flags", ok);
    CHECK("each model's pad is named as Linux names it, and a DualPoint has a stick", named);
}

/** A version 1 pad's and a version 2 DualPoint's set-up, byte by byte, as
 * Linux's alps_hw_init_v1_v2 sends it. */
static void v1_v2_setup_rows(void)
{
    static const uint8_t v1[3] = {0x33, 0x02, 0x0a};
    static const uint8_t v2[3] = {0x20, 0x02, 0x0e};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    static const uint8_t pass_on[4] = {0xE7, 0xE7, 0xE7, 0xF5};
    static const uint8_t pass_off[4] = {0xE6, 0xE6, 0xE6, 0xF5};
    static const uint8_t tap[9] = {0xE9, 0xF5, 0xF5, 0xF3, 0x0A, 0xF5, 0xF5, 0xF5, 0xE9};
    static const uint8_t absolute[7] = {0xF5, 0xF5, 0xF5, 0xF5, 0xF4, 0xF0, 0xEA};
    static const uint8_t stick_saw[9] = {0xE9, 0xF5, 0xF5, 0xF3, 0x0A, 0xF5, 0xF5, 0xF5, 0xE9};

    alps_model(v1, ec, 0);
    CHECK("a version 1 pad is brought up", bring_up() == 0 && alps == 1);
    expect_probe();
    expect_n(tap, 9);
    expect_n(absolute, 7);
    expect_initialize();
    CHECK("its set-up is Linux's: tapping, the absolute knock, stream mode", took_expected());
    CHECK("it took the absolute knock and reports", v12_abs && reporting);
    CHECK("its packets are six bytes of 0x88 under 0xf8",
          pktsize == 6 && priv.byte0 == 0x88 && priv.mask0 == 0xf8);
    CHECK("the bring-up turns interrupts on again, the handler registered",
          irq_on && registered_handler == mouse_irq_handler);
    CHECK("no PS/2 mouse device is registered beside it", mouse_dev == NULL);

    alps_model(v2, ec, 0);
    CHECK("a version 2 DualPoint is brought up", bring_up() == 0 && alps == 1);
    expect_probe();
    expect_n(pass_on, 4);
    expect_n(tap, 9);
    expect_n(absolute, 6);
    expect_n(pass_off, 4);
    expect_n(absolute + 6, 1);
    expect_initialize();
    CHECK("its set-up reaches the pad through the stick", took_expected());
    CHECK("the pad behind the stick took tapping and the absolute knock",
          stick_took_first(stick_saw, 9) && !v2pass);
}

/*
 * The version 3 to 8 set-ups, by the registers they leave.
 */

/** A version 3 Pinnacle DualPoint, its stick seen in register 8. */
static void v3_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x02, 0x64};
    static const uint8_t ec[3] = {0x88, 0x07, 0x9d};

    alps_model(e7, ec, 3);
    regs[0x0008] = 0x80;
    pass_reg = 0x0008;
}

/** A version 3 Rushmore DualPoint, its stick seen in register 0xc2c8. */
static void rushmore_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x03, 0x0a};
    static const uint8_t ec[3] = {0x88, 0x08, 0x1d};

    alps_model(e7, ec, 3);
    regs[0xc2c8] = 0x80;
    regs[0xc2c6] = 0x02;
    pass_reg = 0xc2c8;
}

/** A version 4 pad, by its EC report's last byte. */
static void v4_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x02, 0x64};
    static const uint8_t ec[3] = {0x73, 0x01, 0x8a};

    alps_model(e7, ec, 4);
}

/** A version 5 Dolphin pad of 23 by 12 electrodes. */
static void v5_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x03, 0x50};
    static const uint8_t ec[3] = {0x73, 0x02, 0x50};

    alps_model(e7, ec, 3);
    regs[0x0033] = 0xbf; /* 8 + 15 X electrodes, 1 + 11 Y */
}

/** A version 6 pad, the Dell XT2's, a row of the table. */
static void v6_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x00, 0x14};
    static const uint8_t ec[3] = {0x73, 0x00, 0x00};

    alps_model(e7, ec, 0);
}

/** A version 7 clickpad with a stick, firmware 0xb3. */
static void v7_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x03, 0x0a};
    static const uint8_t ec[3] = {0x88, 0xb3, 0x22};

    alps_model(e7, ec, 3);
    regs[0xc2c8] = 0x80;
}

/** A version 8 SS4 clickpad of 12 by 11 electrodes at a 5 mm pitch. */
static void ss4_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x03, 0x14};
    static const uint8_t ec[3] = {0x73, 0x01, 0x0a};

    alps_model(e7, ec, 3);
    model.otp1[0] = 0x45;
    model.otp1[1] = 0x08; /* a clickpad */
}

/** A version 8 SS5 (SS4 plus) DualPoint of 18 by 10 electrodes at 3.7 mm,
 * its stick in its OTP bytes. */
static void ss5_setup(void)
{
    static const uint8_t e7[3] = {0x73, 0x03, 0x28};
    static const uint8_t ec[3] = {0x73, 0x01, 0x0a};

    alps_model(e7, ec, 3);
    model.otp0[0] = 0x10; /* a stick */
    model.otp0[2] = 0x52;
}

/* The stick's E7 report and extended mode, as a DualPoint's pad passes
 * them on. */
static const uint8_t extended[11] = {0xE7, 0xE7, 0xE7, 0xE9, 0xE6, 0xE6,
                                     0xE6, 0xF3, 0xC8, 0xF3, 0x14};

/** A version 3 pad's set-up, with a stick and without. */
static void v3_setup_rows(void)
{
    v3_setup();
    CHECK("a version 3 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is version 3, a DualPoint with pressure",
          priv.proto_version == PROTO_V3 && priv.flags == (F_DUALPOINT | F_STICK_PRESSURE));
    CHECK("its stick took the E7 report and its extended mode through the pad",
          stick_took_first(extended, 11));
    CHECK("the pad is in absolute mode", regs[0x0004] == 0x06);
    CHECK("its registers are as Linux sets them",
          regs[0x0006] == 0x01 && regs[0x0007] == 0x01 && regs[0x0144] == 0x04 &&
              regs[0x0159] == 0x03 && regs[0x0163] == 0x03 && regs[0x0162] == 0x04);
    CHECK("pass-through is off and the pad knows the stick is extended", regs[0x0008] == 0x82);
    CHECK("it left command mode and reports", !cmd_mode && reporting && dev_rate == 100);
    CHECK("its limits are 2000 by 1400 of 15 by 11 electrodes",
          priv.x_max == 2000 && priv.y_max == 1400 && priv.x_bits == 15 && priv.y_bits == 11);

    v3_setup();
    regs[0x0008] = 0x00;
    CHECK("a version 3 pad without a stick is brought up", bring_up() == 0 && alps == 1);
    CHECK("as a GlidePoint with no stick", priv.flags == F_STICK_PRESSURE && stick == 0 &&
                                               open_named(&n_pad, "AlpsPS/2 ALPS GlidePoint"));
    CHECK("and no command reached a stick", n_stick_took == 0);
}

/** A Rushmore pad's set-up and a version 4 pad's. */
static void rushmore_v4_setup_rows(void)
{
    static const uint8_t v4_knock[10] = {0xF3, 0xC8, 0xF3, 0x64, 0xF3,
                                         0x50, 0xF2, 0xF3, 0x64, 0xF4};
    int i;

    rushmore_setup();
    CHECK("a Rushmore pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is version 3 Rushmore", priv.proto_version == PROTO_V3_RUSHMORE &&
                                          priv.flags == (F_DUALPOINT | F_STICK_PRESSURE));
    CHECK("its stick is extended", stick_took_first(extended, 11) && regs[0xc2c8] == 0x82);
    CHECK("its registers are Linux's", regs[0xc2cb] == 0x00 && regs[0xc2c6] == 0x00 &&
                                           regs[0xc2c9] == 0x64 && regs[0xc2c4] == 0x02);
    CHECK("its resolution comes from its pitch, 25 and 32 a millimetre",
          priv.x_res == 25 && priv.y_res == 32 && priv.x_bits == 16 && priv.y_bits == 12);

    v4_setup();
    CHECK("a version 4 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is version 4, eight-byte packets",
          priv.proto_version == PROTO_V4 && priv.flags == 0 && pktsize == 8);
    CHECK("it is in absolute mode", regs[0x0004] == 0x02);
    CHECK("its registers are Linux's", regs[0x0007] == 0x8c && regs[0x0149] == 0x03 &&
                                           regs[0x0160] == 0x03 && regs[0x017f] == 0x15 &&
                                           regs[0x0151] == 0x01 && regs[0x0168] == 0x03 &&
                                           regs[0x014a] == 0x03 && regs[0x0161] == 0x03);
    for (i = 0; i < 10 && n_took >= 16; i++) {
        if (took[n_took - 16 + i] != v4_knock[i]) {
            break;
        }
    }
    CHECK("then the knock of eight-byte packets and the enable", i == 10);
}

/** A version 5 pad's set-up and a version 6 pad's. */
static void v5_v6_setup_rows(void)
{
    v5_setup();
    CHECK("a version 5 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is version 5 with its electrodes from register 0x33",
          priv.proto_version == PROTO_V5 && priv.x_bits == 23 && priv.y_bits == 12 &&
              priv.x_max == 1408 && priv.y_max == 704);

    v5_setup();
    regs[0x0033] = 0x0f; /* 8 + 15 X electrodes, 1 + 0 Y */
    CHECK("a version 5 pad of one Y electrode is not driven as ALPS", bring_up() == 0 && alps == 0);

    v6_setup();
    CHECK("a version 6 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is version 6, a DualPoint without pressure",
          priv.proto_version == PROTO_V6 && priv.flags == F_DUALPOINT);
    CHECK("its stick took its extended mode through the pad", stick_took_first(extended + 4, 7));
    CHECK("monitor mode wrote register 0 with 0x181",
          mon_writes == 1 && mon_word0 == 0x0A0 && mon_addr == 0 && mon_value == 0x181);
    CHECK("and monitor mode and pass-through were left", !mon_mode && !v2pass);
}

/** A version 7 pad's set-up, a clickpad with a stick and a pad of buttons
 * without one. */
static void v7_setup_rows(void)
{
    v7_setup();
    CHECK("a version 7 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is a version 7 clickpad with a stick",
          priv.proto_version == PROTO_V7 &&
              priv.flags == (F_DUALPOINT | F_STICK_PRESSURE | F_BUTTONPAD));
    CHECK("its registers are Linux's", regs[0xc2c9] == 0x64 && regs[0xc2c4] == 0x02);
    CHECK("its limits and resolution are Linux's",
          priv.x_max == 0xfff && priv.y_max == 0x7ff && priv.x_res == 51 && priv.y_res == 47);

    v7_setup();
    model.ec[1] = 0xba;
    regs[0xc2c8] = 0x00;
    CHECK("a version 7 pad of firmware 0xba without a stick is brought up",
          bring_up() == 0 && alps == 1);
    CHECK("it has buttons and no stick", priv.flags == F_STICK_PRESSURE && stick == 0);
}

/** The version 8 pads' set-up: an SS4 clickpad, an SS5 DualPoint, and an
 * SS5 whose stick is in a register. */
static void v8_setup_rows(void)
{
    ss4_setup();
    CHECK("an SS4 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is a version 8 clickpad",
          priv.proto_version == PROTO_V8 && priv.flags == F_BUTTONPAD);
    CHECK("of Linux's size and resolution",
          priv.x_max == 2816 && priv.y_max == 2560 && priv.x_res == 51 && priv.y_res == 51);
    CHECK("its noise register is written", regs[0x001d] == 0x20 && !cmd_mode && reporting);

    ss5_setup();
    CHECK("an SS5 pad is brought up", bring_up() == 0 && alps == 1);
    CHECK("it is a version 8 DualPoint",
          priv.proto_version == PROTO_V8 && priv.flags == (F_DUALPOINT | F_STICK_PRESSURE));
    CHECK("of Linux's size and resolution",
          priv.x_max == 2176 && priv.y_max == 1152 && priv.x_res == 34 && priv.y_res == 34);

    ss5_setup();
    model.otp0[0] = 0;
    regs[0x00d7] = 0x0c;
    CHECK("an SS5 pad of a ThinkPad L or E series is brought up", bring_up() == 0 && alps == 1);
    CHECK("its stick is found in register 0xd7", (priv.flags & F_DUALPOINT) && stick != 0);
}

/** A version 9 pad and the CS19 stick, which Linux leaves to others. */
static void left_alone_rows(void)
{
    static const uint8_t v9_e7[3] = {0x73, 0x03, 0xc8};
    static const uint8_t v9_ec[3] = {0x73, 0x01, 0x0a};
    static const uint8_t cs19_e7[3] = {0x20, 0x02, 0x0e};
    uint8_t props[8] = {0};

    alps_model(v9_e7, v9_ec, 3);
    CHECK("a version 9 pad is left to the PS/2 driver", bring_up() == 0 && alps == 0);
    CHECK("with Linux's warning", warned("unsupported ALPS V9 touchpad"));
    CHECK("as PS/2 Generic Mouse", mouse_dev != NULL && pad == 0);

    alps_model(cs19_e7, v9_ec, 0);
    cs19 = 1;
    CHECK("the CS19 stick is left to the TrackPoint driver",
          bring_up() == 0 && alps == 0 && pad == 0 && protocol == PROTO_TRACKPOINT);
    CHECK("which names it as Linux's does, a pointing stick",
          open_named(&n_bare, "TPPS/2 ALPS TrackPoint") &&
              evdev_ioctl(&n_bare.e, EVIOCGPROP(8), (uintptr_t)props) > 0 &&
              (props[0] & (1u << INPUT_PROP_POINTING_STICK)));
    close_node(&n_bare);
}

/** A plain mouse, a Synaptics pad, a pad whose set-up fails, and a pad
 * that answers the reports with a rate of 10. */
static void probe_rows(void)
{
    static const uint8_t v1[3] = {0x33, 0x02, 0x0a};
    static const uint8_t v2[3] = {0x20, 0x02, 0x0e};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    static const uint8_t trial[24] = {0xFF, 0xF6, 0xF6, 0xE8, 0x00, 0xE6, 0xE6, 0xE6,
                                      0xE9, 0xE8, 0x00, 0xE7, 0xE7, 0xE7, 0xE9, 0xE8,
                                      0x00, 0xEC, 0xEC, 0xEC, 0xE9, 0xEA, 0xE1, 0xF3};
    char name[64];
    int i;

    model_reset(KIND_PLAIN);
    CHECK("a plain mouse's bring-up succeeds", bring_up() == 0);
    for (i = 0; i < 24 && i < n_took; i++) {
        if (took[i] != trial[i]) {
            break;
        }
    }
    CHECK("it is asked the three reports and a TrackPoint's ID, and then knocked as a wheel mouse",
          i == 24);
    CHECK("it is no ALPS pad", alps == 0 && pad == 0 && stick == 0);
    CHECK("and is asked for an Elantech pad after", elantech_asked == 1);
    CHECK("it is PS/2 Generic Mouse", open_named(&n_pad, "PS/2 Generic Mouse"));

    alps_model(v1, ec, 0);
    synaptics_answer = -1;
    CHECK("a pad that answered Synaptics's knock is brought up", bring_up() == 0);
    CHECK("and is not asked ALPS's reports", alps == 0 && n_took < 20 && pad == 0);
    CHECK("nor Elantech's", elantech_asked == 0);

    alps_model(v1, ec, 0);
    model.e6 = 10;
    CHECK("an E6 report of rate 10 is ALPS's too", bring_up() == 0 && alps == 1);
    CHECK("and an ALPS pad is not asked for an Elantech pad", elantech_asked == 0);

    alps_model(v1, ec, 0);
    refuse_arg = 0x0a;
    CHECK("a pad that refuses its tapping is brought up", bring_up() == 0);
    CHECK("as a mouse whose ALPS set-up failed", alps == -1 && pad == 0 && stick == 0);
    CHECK("which is not asked for an Elantech pad", elantech_asked == 0);
    CHECK("its failure is logged",
          warned("failed to enable hardware tapping") && warned("the touchpad refused its set-up"));
    CHECK("it is PS/2 ALPS GlidePoint",
          open_named(&n_pad, "PS/2 ALPS GlidePoint") &&
              evdev_ioctl(&n_pad.e, EVIOCGNAME(64), (uintptr_t)name) > 0);
    take(&n_pad);
    irq_byte(0x09);
    irq_byte(0x05);
    irq_byte(0x00);
    CHECK("its packets are a mouse's", take(&n_pad) > 0 && n_got == 3 && got[0].code == BTN_LEFT &&
                                           got[1].code == REL_X && got[1].value == 5);

    alps_model(v2, ec, 0);
    refuse_arg = 0x0a;
    CHECK("a DualPoint whose set-up fails is PS/2 ALPS DualPoint TouchPad",
          bring_up() == 0 && alps == -1 && open_named(&n_pad, "PS/2 ALPS DualPoint TouchPad"));
}

/*
 * Capabilities, against what Linux's alps_init, input_mt_init_slots and
 * psmouse give each version.
 */

/** Whether the pad's event device is named `name`, version `version`, with
 * the properties, event types, keys and axes given. */
static int pad_is(const char *name, uint16_t version, const char *prop, const char *ev,
                  const char *key, const char *absb)
{
    if (!open_named(&n_pad, name)) {
        printf("alps-test: no event device %s\n", name);
        return 0;
    }
    return ident_is(&n_pad, "isa0060/serio1/input0", PSMOUSE_ALPS, version) &&
           bits_are(&n_pad, B_PROP, prop) && bits_are(&n_pad, B_EV, ev) &&
           bits_are(&n_pad, B_KEY, key) && bits_are(&n_pad, B_ABS, absb);
}

/** Whether the pad's slots' axes are Linux's for limits `x_max`, `y_max`
 * and resolutions `x_res`, `y_res`. */
static int slots_are(int x_max, int y_max, int x_res, int y_res)
{
    return abs_is(&n_pad, ABS_MT_SLOT, 0, 3, 0) &&
           abs_is(&n_pad, ABS_MT_TRACKING_ID, 0, 65535, 0) &&
           abs_is(&n_pad, ABS_MT_POSITION_X, 0, x_max, x_res) &&
           abs_is(&n_pad, ABS_MT_POSITION_Y, 0, y_max, y_res) &&
           abs_is(&n_pad, ABS_X, 0, x_max, x_res) && abs_is(&n_pad, ABS_Y, 0, y_max, y_res);
}

/** Each version's capabilities. */
static void capability_rows(void)
{
    static const uint8_t wheel[3] = {0x63, 0x02, 0x3c};
    static const uint8_t four[3] = {0x73, 0x02, 0x50};
    static const uint8_t fwbk[3] = {0x63, 0x02, 0x28};
    static const uint8_t v2[3] = {0x20, 0x02, 0x0e};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};

    alps_model(v2, ec, 0);
    bring_up();
    CHECK("a version 2 DualPoint's pad is Linux's",
          pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x200, "1", "b", "420 70000 0 0 0 0",
                 "1000003") &&
              abs_is(&n_pad, ABS_X, 0, 1023, 0) && abs_is(&n_pad, ABS_Y, 0, 767, 0) &&
              abs_is(&n_pad, ABS_PRESSURE, 0, 127, 0) && bits_are(&n_pad, B_REL, "0"));
    CHECK("its stick is Linux's, without pressure", stick_is(0x200, 0));

    alps_model(wheel, ec, 0);
    bring_up();
    CHECK("a pad with a wheel has REL_WHEEL",
          pad_is("AlpsPS/2 ALPS GlidePoint", 0x200, "1", "f", "420 70000 0 0 0 0", "1000003") &&
              bits_are(&n_pad, B_REL, "100"));
    alps_model(four, ec, 0);
    bring_up();
    CHECK("a pad of four buttons has BTN_0 to BTN_3 and no BTN_MIDDLE",
          pad_is("AlpsPS/2 ALPS GlidePoint", 0x200, "1", "b", "420 3000f 0 0 0 0", "1000003"));
    alps_model(fwbk, ec, 0);
    bring_up();
    CHECK("a pad of forward and back buttons has them",
          pad_is("AlpsPS/2 ALPS GlidePoint", 0x200, "1", "b", "420 670000 0 0 0 0", "1000003"));

    v3_setup();
    bring_up();
    CHECK("a version 3 pad is semi-MT, four slots",
          pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x300, "9", "b", "e420 70000 0 0 0 0",
                 "260800001000003") &&
              slots_are(2000, 1400, 0, 0) && abs_is(&n_pad, ABS_PRESSURE, 0, 127, 0));
    CHECK("its stick has pressure", stick_is(0x300, 1));

    rushmore_setup();
    bring_up();
    CHECK("a Rushmore pad has its resolution",
          pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x310, "9", "b", "e420 70000 0 0 0 0",
                 "260800001000003") &&
              slots_are(2000, 1400, 25, 32));
    CHECK("its stick has pressure", stick_is(0x310, 1));

    v4_setup();
    bring_up();
    CHECK("a version 4 pad is semi-MT", pad_is("AlpsPS/2 ALPS GlidePoint", 0x400, "9", "b",
                                               "e420 70000 0 0 0 0", "260800001000003") &&
                                            slots_are(2000, 1400, 0, 0) && stick == 0);

    v5_setup();
    bring_up();
    CHECK("a version 5 pad is semi-MT", pad_is("AlpsPS/2 ALPS GlidePoint", 0x500, "9", "b",
                                               "e420 70000 0 0 0 0", "260800001000003") &&
                                            slots_are(1408, 704, 0, 0));

    v6_setup();
    bring_up();
    CHECK("a version 6 pad has one finger", pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x600, "1",
                                                   "b", "420 70000 0 0 0 0", "1000003") &&
                                                abs_is(&n_pad, ABS_X, 0, 2047, 0) &&
                                                abs_is(&n_pad, ABS_Y, 0, 1535, 0));
    CHECK("its stick has no pressure", stick_is(0x600, 0));

    v7_setup();
    bring_up();
    CHECK("a version 7 clickpad tracks four contacts, without pressure",
          pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x700, "5", "b", "e520 10000 0 0 0 0",
                 "260800000000003") &&
              slots_are(0xfff, 0x7ff, 51, 47));
    CHECK("its stick has pressure", stick_is(0x700, 1));

    ss4_setup();
    bring_up();
    CHECK("an SS4 clickpad tracks four contacts, with pressure",
          pad_is("AlpsPS/2 ALPS GlidePoint", 0x800, "5", "b", "e520 10000 0 0 0 0",
                 "260800001000003") &&
              slots_are(2816, 2560, 51, 51) && abs_is(&n_pad, ABS_PRESSURE, 0, 127, 0));

    ss5_setup();
    bring_up();
    CHECK("an SS5 DualPoint has its buttons",
          pad_is("AlpsPS/2 ALPS DualPoint TouchPad", 0x800, "1", "b", "e520 70000 0 0 0 0",
                 "260800001000003") &&
              slots_are(2176, 1152, 34, 34));
    CHECK("its stick has pressure", stick_is(0x800, 1));
}

/*
 * Packets: each version's, as alps.rst lays them out, with the events,
 * slots and mouse packets Linux 6.12's alps.c, input-mt and mousedev give.
 */

#define EK(c, v)  {EV_KEY, c, v}
#define EAB(c, v) {EV_ABS, c, v}
#define ERL(c, v) {EV_REL, c, v}
#define ESYN      {EV_SYN, SYN_REPORT, 0}

/** Whether the mouse device has one packet, of `dx`, `dy` and `buttons`. */
static int mouse_got(int dx, int dy, int buttons)
{
    struct mouse_packet p[8];
    int n = mouse_packets(p, 8);

    if (n != 1 || p[0].dx != dx || p[0].dy != dy || p[0].buttons != buttons) {
        printf("alps-test: the mouse device has %d packets, the first %d %d %d\n", n,
               n ? p[0].dx : 0, n ? p[0].dy : 0, n ? p[0].buttons : 0);
        return 0;
    }
    return 1;
}

/** A version 1 pad: a touch, a lift with a button, a hardware tap, a tap
 * and drag, and packets that break their form or a pause cuts. */
static void v1_packet_rows(void)
{
    static const uint8_t v1[3] = {0x33, 0x02, 0x0a};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    /* x 500, y 300, z 50, fin */
    static const uint8_t touch[6] = {0x8B, 0x74, 0x02, 0x02, 0x2C, 0x32};
    static const struct event touch_ev[] = {EK(BTN_TOUCH, 1),       EAB(ABS_X, 500),
                                            EAB(ABS_Y, 300),        EAB(ABS_PRESSURE, 50),
                                            EK(BTN_TOOL_FINGER, 1), ESYN};
    static const uint8_t lift[6] = {0x88, 0x00, 0x10, 0x00, 0x00, 0x00}; /* l */
    static const struct event lift_ev[] = {EK(BTN_LEFT, 1), EK(BTN_TOUCH, 0), EAB(ABS_PRESSURE, 0),
                                           EK(BTN_TOOL_FINGER, 0), ESYN};
    static const uint8_t tap[6] = {0x8B, 0x74, 0x01, 0x02, 0x2C, 0x00}; /* ges, no fin */
    static const struct event tap_ev[] = {EK(BTN_LEFT, 0), EK(BTN_TOUCH, 1), EAB(ABS_PRESSURE, 40),
                                          EK(BTN_TOOL_FINGER, 1), ESYN};
    static const uint8_t drag[6] = {0x8B, 0x74, 0x03, 0x02, 0x2C, 0x32}; /* ges and fin */
    static const struct event drag_ev[] = {EAB(ABS_PRESSURE, 0),  EK(BTN_TOOL_FINGER, 0), ESYN,
                                           EAB(ABS_PRESSURE, 50), EK(BTN_TOOL_FINGER, 1), ESYN};
    static const uint8_t broken[3] = {0x8B, 0x74, 0x80};
    static const uint8_t after[6] = {0x8B, 0x74, 0x02, 0x02, 0x2C, 0x37};
    static const struct event after_ev[] = {EAB(ABS_PRESSURE, 55), ESYN};
    static const uint8_t paused[6] = {0x8B, 0x74, 0x02, 0x02, 0x2C, 0x3A};
    static const struct event paused_ev[] = {EAB(ABS_PRESSURE, 58), ESYN};
    static const uint8_t ps2[3] = {0x08, 0x01, 0x00};

    alps_model(v1, ec, 0);
    bring_up();
    CHECK("a version 1 pad's event device opens", open_named(&n_pad, "AlpsPS/2 ALPS GlidePoint"));
    take(&n_pad);
    send(touch, 6);
    CHECK("a touch is the finger's place and pressure", reads(&n_pad, touch_ev, NEV(touch_ev)));
    send(lift, 6);
    CHECK("a lift with the left button", reads(&n_pad, lift_ev, NEV(lift_ev)));
    send(tap, 6);
    CHECK("a hardware tap is a touch of pressure 40", reads(&n_pad, tap_ev, NEV(tap_ev)));
    send(drag, 6);
    CHECK("a tap and drag gives the lift between", reads(&n_pad, drag_ev, NEV(drag_ev)));

    send(broken, 3);
    CHECK("a byte with bit 7 drops the packet", pktcnt == 0 && out_of_sync == 1);
    send(after, 6);
    CHECK("the packet after it is read", reads(&n_pad, after_ev, NEV(after_ev)) && !out_of_sync);
    send(after, 3);
    kernel_ticks += MS_TO_TICKS(600);
    send(paused, 6);
    CHECK("a packet a pause cuts is dropped, and the next read",
          sync_losses == 1 && reads(&n_pad, paused_ev, NEV(paused_ev)));

    send(ps2, 3);
    CHECK("a mouse's packet on the external port registers PS/2 ALPS Mouse",
          bare != 0 && open_named(&n_bare, "PS/2 ALPS Mouse") &&
              ident_is(&n_bare, "isa0060/serio1/input1", PSMOUSE_PS2, 0) &&
              bits_are(&n_bare, B_PROP, "1") && bits_are(&n_bare, B_EV, "7") &&
              bits_are(&n_bare, B_KEY, "70000 0 0 0 0") && bits_are(&n_bare, B_REL, "3"));
    CHECK("and that packet goes nowhere", reads_nothing(&n_bare) && reads_nothing(&n_pad));
}

/** A version 2 DualPoint: the stick's PS/2 packets, a button pressed on the
 * stick and released on the pad, the stick in an absolute packet, and a
 * Dell whose stick buttons have bits of their own. */
static void v2_packet_rows(void)
{
    static const uint8_t v2[3] = {0x20, 0x02, 0x0e};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    static const uint8_t move[3] = {0x28, 0x05, 0xFD}; /* x 5, y -3 */
    static const struct event move_ev[] = {ERL(REL_X, 5), ERL(REL_Y, 3), ESYN};
    static const uint8_t press[3] = {0x09, 0x00, 0x00};
    static const struct event press_ev[] = {EK(BTN_LEFT, 1), ESYN};
    /* x 600, y 400, z 60, fin */
    static const uint8_t touch[6] = {0xF8, 0x58, 0x22, 0x38, 0x10, 0x3C};
    static const struct event release_ev[] = {EK(BTN_LEFT, 0), ESYN};
    static const struct event touch_ev[] = {EK(BTN_TOUCH, 1),       EAB(ABS_X, 600),
                                            EAB(ABS_Y, 400),        EAB(ABS_PRESSURE, 60),
                                            EK(BTN_TOOL_FINGER, 1), ESYN};
    static const uint8_t z127[6] = {0xF8, 0x0A, 0x00, 0x38, 0x74, 0x7F}; /* x 10, y 500 */
    static const struct event z127_ev[] = {ERL(REL_X, 10), ERL(REL_Y, 12), ESYN};
    static const uint8_t psl[6] = {0xF9, 0x58, 0x22, 0x38, 0x10, 0x3C};
    static const struct event psl_ev[] = {EK(BTN_LEFT, 1),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 600),
                                          EAB(ABS_Y, 400),
                                          EAB(ABS_PRESSURE, 60),
                                          EK(BTN_TOOL_FINGER, 1),
                                          ESYN};

    alps_model(v2, ec, 0);
    bring_up();
    CHECK("a DualPoint's pad and stick open",
          open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad") &&
              open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick"));
    take(&n_pad);
    take(&n_stick);
    send(move, 3);
    CHECK("the stick's PS/2 packet moves the stick",
          reads(&n_stick, move_ev, NEV(move_ev)) && reads_nothing(&n_pad));
    CHECK("and the mouse device's pointer", mouse_got(5, -3, 0));
    send(press, 3);
    CHECK("a button in the stick's packet is the stick's",
          reads(&n_stick, press_ev, NEV(press_ev)));
    CHECK("and the mouse device's", mouse_got(0, 0, 1));
    send(touch, 6);
    CHECK("a pad packet releases it on the stick, where it was pressed",
          reads(&n_stick, release_ev, NEV(release_ev)));
    CHECK("and is the pad's touch", reads(&n_pad, touch_ev, NEV(touch_ev)));
    send(z127, 6);
    CHECK("pressure 127 is the stick's motion",
          reads(&n_stick, z127_ev, NEV(z127_ev)) && reads_nothing(&n_pad));

    alps_model(v2, ec, 0);
    set_system("Dell Inc.", "Latitude D620");
    bring_up();
    CHECK("a Latitude D620's pad has its stick buttons in bits of their own",
          (priv.flags & F_STICK_BITS) != 0);
    open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad");
    take(&n_pad);
    send(psl, 6);
    CHECK("and a press of the stick's left button is the pad's",
          reads(&n_pad, psl_ev, NEV(psl_ev)));

    alps_model(v2, ec, 0);
    set_system("Dell Inc.", "Latitude D600");
    bring_up();
    CHECK("a Latitude D600's has not", (priv.flags & F_STICK_BITS) == 0);
}

/** A DualPoint that cuts the stick's packets into the pad's: a packet of
 * the pad, one with the stick's inside, and one whose fourth byte looks
 * like a cut packet's, read when 20 ms pass. */
static void interleaved_rows(void)
{
    static const uint8_t e7[3] = {0x62, 0x02, 0x14};
    static const uint8_t ec[3] = {0x00, 0x00, 0x00};
    static const uint8_t touch[6] = {0xCF, 0x58, 0x22, 0x38, 0x10, 0x3C};
    static const struct event touch_ev[] = {EK(BTN_TOUCH, 1),       EAB(ABS_X, 600),
                                            EAB(ABS_Y, 400),        EAB(ABS_PRESSURE, 60),
                                            EK(BTN_TOOL_FINGER, 1), ESYN};
    /* the pad's first three bytes, the stick's x 4, y 2, the pad's last
     * three, z 61 */
    static const uint8_t cut[9] = {0xCF, 0x58, 0x22, 0x0F, 0x04, 0x02, 0x38, 0x10, 0x3D};
    static const struct event cut_stick_ev[] = {ERL(REL_X, 4), ERL(REL_Y, -2), ESYN};
    static const struct event cut_pad_ev[] = {EAB(ABS_PRESSURE, 61), ESYN};
    static const uint8_t buttons[6] = {0xCF, 0x58, 0x22, 0x3F, 0x10, 0x3D}; /* l, r and m */
    static const struct event buttons_ev[] = {EK(BTN_LEFT, 1), EK(BTN_RIGHT, 1), EK(BTN_MIDDLE, 1),
                                              ESYN};

    alps_model(e7, ec, 0);
    bring_up();
    CHECK("an interleaving DualPoint opens",
          open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad") &&
              open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick"));
    take(&n_pad);
    take(&n_stick);
    send(touch, 6);
    CHECK("its own packet is read at once", reads(&n_pad, touch_ev, NEV(touch_ev)));
    send(cut, 9);
    CHECK("a stick packet cut into the pad's moves the stick",
          reads(&n_stick, cut_stick_ev, NEV(cut_stick_ev)));
    CHECK("and the pad's packet around it is read whole",
          reads(&n_pad, cut_pad_ev, NEV(cut_pad_ev)));
    send(buttons, 6);
    CHECK("a packet that may hold a cut one waits", reads_nothing(&n_pad));
    kernel_ticks += MS_TO_TICKS(10);
    alps_tick();
    CHECK("for 20 ms", reads_nothing(&n_pad));
    kernel_ticks += MS_TO_TICKS(10);
    alps_tick();
    CHECK("then it is read", reads(&n_pad, buttons_ev, NEV(buttons_ev)) && pktcnt == 0);
}

/** A version 3 DualPoint: one finger, two by their bitmaps, the stick, a
 * lift, the stick's buttons in the pad's packet, and the external port. */
static void v3_packet_rows(void)
{
    /* x 1000, y 700, z 50 */
    static const uint8_t one[6] = {0x8F, 0x3E, 0x2B, 0x08, 0x2C, 0x32};
    static const struct event one_ev[] = {EAB(ABS_MT_TRACKING_ID, 0),
                                          EAB(ABS_MT_POSITION_X, 1000),
                                          EAB(ABS_MT_POSITION_Y, 700),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 1000),
                                          EAB(ABS_Y, 700),
                                          EK(BTN_TOOL_FINGER, 1),
                                          EAB(ABS_PRESSURE, 50),
                                          ESYN};
    /* x 400, y 900, z 60, mt; then the bitmaps of X 0x0c0c and Y 0xfc */
    static const uint8_t first[6] = {0x8F, 0x19, 0x38, 0x08, 0x44, 0x3C};
    static const uint8_t bitmap[6] = {0xCF, 0x03, 0x7E, 0x0F, 0x0C, 0x41};
    static const struct event two_ev[] = {EAB(ABS_MT_POSITION_X, 400),
                                          EAB(ABS_MT_POSITION_Y, 900),
                                          EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 1500),
                                          EAB(ABS_MT_POSITION_Y, 770),
                                          EAB(ABS_X, 400),
                                          EAB(ABS_Y, 900),
                                          EK(BTN_TOOL_FINGER, 0),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          EAB(ABS_PRESSURE, 60),
                                          ESYN};
    /* x 40, y -24, z 20 */
    static const uint8_t stick_p[6] = {0xDF, 0x28, 0x68, 0x48, 0x14, 0x3F};
    static const struct event stick_ev[] = {ERL(REL_X, 5), ERL(REL_Y, 3), EAB(ABS_PRESSURE, 20),
                                            ESYN};
    static const uint8_t lift[6] = {0x8F, 0x00, 0x00, 0x08, 0x00, 0x00};
    static const struct event lift_ev[] = {EAB(ABS_MT_SLOT, 0),  EAB(ABS_MT_TRACKING_ID, -1),
                                           EAB(ABS_MT_SLOT, 1),  EAB(ABS_MT_TRACKING_ID, -1),
                                           EK(BTN_TOUCH, 0),     EK(BTN_TOOL_DOUBLETAP, 0),
                                           EAB(ABS_PRESSURE, 0), ESYN};
    static const uint8_t stick_left[6] = {0x8F, 0x00, 0x00, 0x18, 0x00, 0x00};
    static const struct event stick_left_ev[] = {EK(BTN_LEFT, 1), ESYN};
    static const uint8_t ps2[3] = {0x09, 0x05, 0x00};
    static const struct event ps2_ev[] = {EK(BTN_LEFT, 1), ERL(REL_X, 5), ESYN};

    v3_setup();
    bring_up();
    CHECK("a version 3 DualPoint opens", open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad") &&
                                             open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick"));
    take(&n_pad);
    take(&n_stick);
    send(one, 6);
    CHECK("one finger is slot 0 and the pointer", reads(&n_pad, one_ev, NEV(one_ev)));
    CHECK("and the stick's buttons, unchanged, give nothing", reads_nothing(&n_stick));
    send(first, 6);
    CHECK("a place packet with the mt bit waits for the bitmaps", reads_nothing(&n_pad));
    send(bitmap, 6);
    CHECK("the bitmaps give the second contact in the corner across",
          reads(&n_pad, two_ev, NEV(two_ev)));
    mouse_clear_packets();
    send(stick_p, 6);
    CHECK("a stick packet moves the stick an eighth of its count",
          reads(&n_stick, stick_ev, NEV(stick_ev)) && reads_nothing(&n_pad));
    CHECK("and the mouse device's pointer", mouse_got(5, -3, 0));
    send(lift, 6);
    CHECK("a lift ends both slots", reads(&n_pad, lift_ev, NEV(lift_ev)));
    send(stick_left, 6);
    CHECK("the stick's left button in the pad's packet is the stick's",
          reads(&n_stick, stick_left_ev, NEV(stick_left_ev)) && reads_nothing(&n_pad));

    send(ps2, 3);
    CHECK("a mouse's packet registers PS/2 ALPS Mouse after the stick",
          open_named(&n_bare, "PS/2 ALPS Mouse") &&
              ident_is(&n_bare, "isa0060/serio1/input2", PSMOUSE_PS2, 0));
    send(ps2, 3);
    CHECK("its next packet is the mouse's", reads(&n_bare, ps2_ev, NEV(ps2_ev)));
}

/** A Rushmore pad's two contacts, its Y not turned over. */
static void rushmore_packet_rows(void)
{
    /* x 350, y 450, z 40, mt; the bitmaps of X 0x0c0c and Y 0xfc */
    static const uint8_t first[6] = {0xAF, 0x15, 0x1C, 0x08, 0x72, 0x28};
    static const uint8_t bitmap[6] = {0x8F, 0x03, 0x7E, 0x0F, 0x0C, 0x41};
    static const struct event two_ev[] = {EAB(ABS_MT_TRACKING_ID, 0),
                                          EAB(ABS_MT_POSITION_X, 350),
                                          EAB(ABS_MT_POSITION_Y, 450),
                                          EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 1400),
                                          EAB(ABS_MT_POSITION_Y, 572),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 350),
                                          EAB(ABS_Y, 450),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          EAB(ABS_PRESSURE, 40),
                                          ESYN};

    rushmore_setup();
    bring_up();
    open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad");
    take(&n_pad);
    send(first, 6);
    CHECK("a Rushmore place packet with the mt bit waits", reads_nothing(&n_pad));
    send(bitmap, 6);
    CHECK("its bitmaps give two contacts", reads(&n_pad, two_ev, NEV(two_ev)));
}

/** A version 4 pad: three packets, each a place, whose last bytes make the
 * bitmaps. */
static void v4_packet_rows(void)
{
    /* x 400, y 900, z 50; the bitmaps of X 0x0c0c and Y 0xfc, in thirds */
    static const uint8_t p1[8] = {0x8F, 0x19, 0x38, 0x44, 0x08, 0x32, 0x43, 0x1C};
    static const uint8_t p2[8] = {0x8F, 0x19, 0x38, 0x44, 0x08, 0x32, 0x03, 0x07};
    static const uint8_t p3[8] = {0x8F, 0x19, 0x38, 0x44, 0x08, 0x32, 0x00, 0x00};
    static const struct event one_ev[] = {EAB(ABS_MT_TRACKING_ID, 0),
                                          EAB(ABS_MT_POSITION_X, 400),
                                          EAB(ABS_MT_POSITION_Y, 900),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 400),
                                          EAB(ABS_Y, 900),
                                          EK(BTN_TOOL_FINGER, 1),
                                          EAB(ABS_PRESSURE, 50),
                                          ESYN};
    static const struct event two_ev[] = {EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 1500),
                                          EAB(ABS_MT_POSITION_Y, 770),
                                          EK(BTN_TOOL_FINGER, 0),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          ESYN};

    v4_setup();
    bring_up();
    open_named(&n_pad, "AlpsPS/2 ALPS GlidePoint");
    take(&n_pad);
    send(p1, 8);
    CHECK("a version 4 packet is read whole, eight bytes", reads(&n_pad, one_ev, NEV(one_ev)));
    send(p2, 8);
    CHECK("the same place again gives nothing", reads_nothing(&n_pad));
    send(p3, 8);
    CHECK("the third packet completes the bitmaps", reads(&n_pad, two_ev, NEV(two_ev)));
}

/** A version 5 pad: one finger, a lift, and two by its profiles. */
static void v5_packet_rows(void)
{
    /* x 700, y 300, z 40 */
    static const uint8_t one[6] = {0xC8, 0x3C, 0x2C, 0x08, 0x25, 0x28};
    static const struct event one_ev[] = {EAB(ABS_MT_TRACKING_ID, 0),
                                          EAB(ABS_MT_POSITION_X, 700),
                                          EAB(ABS_MT_POSITION_Y, 300),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 700),
                                          EAB(ABS_Y, 300),
                                          EK(BTN_TOOL_FINGER, 1),
                                          EAB(ABS_PRESSURE, 40),
                                          ESYN};
    static const uint8_t lift[6] = {0xC8, 0x00, 0x00, 0x08, 0x00, 0x00};
    static const struct event lift_ev[] = {EAB(ABS_MT_TRACKING_ID, -1), EK(BTN_TOUCH, 0),
                                           EK(BTN_TOOL_FINGER, 0), EAB(ABS_PRESSURE, 0), ESYN};
    /* x 1200, y 230, z 50, first of two; the profiles of X 0xc00c, Y 0xfc */
    static const uint8_t first[6] = {0xCA, 0x30, 0x66, 0x08, 0x19, 0x32};
    static const uint8_t profile[6] = {0xEC, 0x7C, 0x01, 0x00, 0x03, 0x60};
    static const struct event two_ev[] = {EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 1200),
                                          EAB(ABS_MT_POSITION_Y, 230),
                                          EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 2),
                                          EAB(ABS_MT_POSITION_X, 480),
                                          EAB(ABS_MT_POSITION_Y, 288),
                                          EK(BTN_TOUCH, 1),
                                          EAB(ABS_X, 1200),
                                          EAB(ABS_Y, 230),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          EAB(ABS_PRESSURE, 50),
                                          ESYN};

    v5_setup();
    bring_up();
    open_named(&n_pad, "AlpsPS/2 ALPS GlidePoint");
    take(&n_pad);
    send(one, 6);
    CHECK("a version 5 finger is slot 0 and the pointer", reads(&n_pad, one_ev, NEV(one_ev)));
    send(lift, 6);
    CHECK("its lift ends the slot", reads(&n_pad, lift_ev, NEV(lift_ev)));
    send(first, 6);
    CHECK("the first of two packets waits", reads_nothing(&n_pad));
    send(profile, 6);
    CHECK("the profiles give the second contact, X turned over",
          reads(&n_pad, two_ev, NEV(two_ev)));
}

/** A version 6 pad: a touch, the pad's button on the stick's device, and
 * the stick. */
static void v6_packet_rows(void)
{
    /* x 1000, y 800, z 60 */
    static const uint8_t touch[6] = {0xFF, 0x68, 0x20, 0x38, 0x30, 0x3C};
    static const struct event touch_ev[] = {EK(BTN_TOUCH, 1),       EAB(ABS_X, 1000),
                                            EAB(ABS_Y, 800),        EAB(ABS_PRESSURE, 60),
                                            EK(BTN_TOOL_FINGER, 1), ESYN};
    static const uint8_t left[6] = {0xFF, 0x68, 0x20, 0x39, 0x30, 0x3C};
    static const struct event left_ev[] = {EK(BTN_LEFT, 1), ESYN};
    /* x 20, y -12, z 10 */
    static const uint8_t stick_p[6] = {0xFF, 0x14, 0x74, 0x40, 0x0A, 0x7F};
    static const struct event stick_ev[] = {ERL(REL_X, 5), ERL(REL_Y, 3), EK(BTN_LEFT, 0), ESYN};

    v6_setup();
    bring_up();
    CHECK("a version 6 pad opens", open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad") &&
                                       open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick"));
    take(&n_pad);
    take(&n_stick);
    send(touch, 6);
    CHECK("a touch is the finger's place", reads(&n_pad, touch_ev, NEV(touch_ev)));
    send(left, 6);
    CHECK("the pad's button is the stick device's, its packet ended",
          reads(&n_stick, left_ev, NEV(left_ev)) && reads_nothing(&n_pad));
    send(stick_p, 6);
    CHECK("a stick packet moves the stick a quarter of its count",
          reads(&n_stick, stick_ev, NEV(stick_ev)));
}

/** A version 7 clickpad: one contact, two, three, a lift, the stick, and a
 * packet of the kind Linux does not trust. */
static void v7_packet_rows(void)
{
    /* contact 0 at x 2000, y 1000; contact 1 the false touch */
    static const uint8_t one[6] = {0x4F, 0x82, 0x7E, 0xE8, 0xFF, 0xBF};
    static const struct event one_ev[] = {EAB(ABS_MT_TRACKING_ID, 0),
                                          EAB(ABS_MT_POSITION_X, 2000),
                                          EAB(ABS_MT_POSITION_Y, 1000),
                                          EK(BTN_TOUCH, 1),
                                          EK(BTN_TOOL_FINGER, 1),
                                          EAB(ABS_X, 2000),
                                          EAB(ABS_Y, 1000),
                                          ESYN};
    /* contact 1 at x 2992, y 496 */
    static const uint8_t two[6] = {0x4F, 0x82, 0x7E, 0xE8, 0x7B, 0xA0};
    static const struct event two_ev[] = {EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 2992),
                                          EAB(ABS_MT_POSITION_Y, 496),
                                          EK(BTN_TOOL_FINGER, 0),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          ESYN};
    /* three fingers, contact 1 at x 2944, y 480 */
    static const uint8_t three[6] = {0x4F, 0x82, 0x7E, 0xE8, 0x39, 0xA0};
    static const struct event three_ev[] = {EAB(ABS_MT_POSITION_X, 2944),
                                            EAB(ABS_MT_POSITION_Y, 480), EK(BTN_TOOL_DOUBLETAP, 0),
                                            EK(BTN_TOOL_TRIPLETAP, 1), ESYN};
    static const uint8_t idle[6] = {0x48, 0x00, 0x40, 0x48, 0x00, 0x00};
    static const struct event idle_ev[] = {EAB(ABS_MT_SLOT, 0),
                                           EAB(ABS_MT_TRACKING_ID, -1),
                                           EAB(ABS_MT_SLOT, 1),
                                           EAB(ABS_MT_TRACKING_ID, -1),
                                           EK(BTN_TOUCH, 0),
                                           EK(BTN_TOOL_TRIPLETAP, 0),
                                           ESYN};
    /* x 10, y -6, z 30 */
    static const uint8_t stick_p[6] = {0x48, 0xC8, 0x4A, 0x6A, 0xBE, 0x1E};
    static const struct event stick_ev[] = {ERL(REL_X, 10), ERL(REL_Y, 6), EAB(ABS_PRESSURE, 30),
                                            ESYN};
    static const uint8_t fresh[6] = {0x58, 0x82, 0x7E, 0xE8, 0x3C, 0xA0};

    v7_setup();
    bring_up();
    CHECK("a version 7 pad opens", open_named(&n_pad, "AlpsPS/2 ALPS DualPoint TouchPad") &&
                                       open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick"));
    take(&n_pad);
    take(&n_stick);
    send(one, 6);
    CHECK("one contact, the false second dropped", reads(&n_pad, one_ev, NEV(one_ev)));
    send(two, 6);
    CHECK("a second contact takes slot 1", reads(&n_pad, two_ev, NEV(two_ev)));
    send(three, 6);
    CHECK("three fingers, two of them placed", reads(&n_pad, three_ev, NEV(three_ev)));
    send(idle, 6);
    CHECK("an idle packet lifts them", reads(&n_pad, idle_ev, NEV(idle_ev)));
    send(stick_p, 6);
    CHECK("a stick packet moves the stick",
          reads(&n_stick, stick_ev, NEV(stick_ev)) && reads_nothing(&n_pad));
    send(fresh, 6);
    CHECK("a NEW packet is dropped, as Linux drops it", reads_nothing(&n_pad));
}

/** A version 8 SS4 clickpad: one contact, idle, two, three across two
 * packets, and a stick packet it should not send; an SS5's stick. */
static void ss4_packet_rows(void)
{
    /* x 1400, y 1200, z 25 */
    static const uint8_t one[6] = {0x18, 0x5F, 0x20, 0xC8, 0x32, 0x29};
    static const struct event one_ev[] = {
        EAB(ABS_MT_TRACKING_ID, 0), EAB(ABS_MT_POSITION_X, 1400), EAB(ABS_MT_POSITION_Y, 1200),
        EK(BTN_TOUCH, 1),           EK(BTN_TOOL_FINGER, 1),       EAB(ABS_X, 1400),
        EAB(ABS_Y, 1200),           EAB(ABS_PRESSURE, 50),        ESYN};
    static const uint8_t idle[6] = {0x18, 0x10, 0x00, 0x08, 0x10, 0x00};
    static const struct event idle_ev[] = {EAB(ABS_MT_TRACKING_ID, -1), EK(BTN_TOUCH, 0),
                                           EK(BTN_TOOL_FINGER, 0), EAB(ABS_PRESSURE, 0), ESYN};
    /* A at x 1024, y 512, pressed; B at x 2048, y 1536 */
    static const uint8_t two[6] = {0x18, 0x21, 0x20, 0x18, 0x40, 0x60};
    static const struct event two_ev[] = {EAB(ABS_MT_TRACKING_ID, 1),
                                          EAB(ABS_MT_POSITION_X, 1024),
                                          EAB(ABS_MT_POSITION_Y, 512),
                                          EAB(ABS_MT_SLOT, 1),
                                          EAB(ABS_MT_TRACKING_ID, 2),
                                          EAB(ABS_MT_POSITION_X, 2048),
                                          EAB(ABS_MT_POSITION_Y, 1536),
                                          EK(BTN_TOUCH, 1),
                                          EK(BTN_TOOL_DOUBLETAP, 1),
                                          EAB(ABS_X, 1024),
                                          EAB(ABS_Y, 512),
                                          EAB(ABS_PRESSURE, 48),
                                          ESYN};
    /* the same, more to follow; then C at x 512, y 1024 and no fourth */
    static const uint8_t cont[6] = {0x18, 0x21, 0x30, 0x18, 0x40, 0x60};
    static const uint8_t multi[6] = {0x18, 0x10, 0x40, 0xFF, 0xFA, 0xEF};
    static const struct event three_ev[] = {EAB(ABS_MT_SLOT, 2),
                                            EAB(ABS_MT_TRACKING_ID, 3),
                                            EAB(ABS_MT_POSITION_X, 512),
                                            EAB(ABS_MT_POSITION_Y, 1024),
                                            EK(BTN_TOOL_DOUBLETAP, 0),
                                            EK(BTN_TOOL_TRIPLETAP, 1),
                                            ESYN};
    /* x 7, y -5, z 15 */
    static const uint8_t stick_p[6] = {0x18, 0x07, 0x7B, 0x29, 0x0F, 0x00};
    static const struct event stick_ev[] = {ERL(REL_X, 7), ERL(REL_Y, 5), EAB(ABS_PRESSURE, 15),
                                            ESYN};

    ss4_setup();
    bring_up();
    open_named(&n_pad, "AlpsPS/2 ALPS GlidePoint");
    take(&n_pad);
    send(one, 6);
    CHECK("an SS4 contact is slot 0, the pointer and pressure", reads(&n_pad, one_ev, NEV(one_ev)));
    send(idle, 6);
    CHECK("an idle packet lifts it", reads(&n_pad, idle_ev, NEV(idle_ev)));
    send(two, 6);
    CHECK("two contacts take slots 0 and 1", reads(&n_pad, two_ev, NEV(two_ev)));
    send(cont, 6);
    CHECK("a packet of two that says more follow waits", reads_nothing(&n_pad));
    send(multi, 6);
    CHECK("the packet after it adds the third contact", reads(&n_pad, three_ev, NEV(three_ev)));
    send(stick_p, 6);
    CHECK("a stick packet from a pad without a stick is dropped and logged",
          reads_nothing(&n_pad) && warned("rejected trackstick packet"));

    ss5_setup();
    bring_up();
    open_named(&n_stick, "AlpsPS/2 ALPS DualPoint Stick");
    take(&n_stick);
    send(stick_p, 6);
    CHECK("an SS5's stick packet moves its stick", reads(&n_stick, stick_ev, NEV(stick_ev)));
}

/* The bytes the bring-up sent, which a reconnect sends again from the
 * reset that follows the CS19 check. */
static uint8_t bringup_took[TOOK_MAX];
static int n_bringup_took;

/* The pad's registers at power-on, which its reset restores and the
 * model's reset leaves as they were. */
static uint8_t power_on_regs[sizeof(regs)];

/** Reconnect the pad as kpsmoused does, its registers at their power-on
 * values as its reset leaves them. */
static void reconnect_pad(void)
{
    k_memcpy(regs, power_on_regs, sizeof(regs));
    n_took = 0;
    ps2mouse_reconnect();
}

/** Whether the device took, since `n_took` was cleared, the bring-up's
 * bytes from its reset after the CS19 check: Linux's `alps_reconnect`, its
 * reset, `alps_identify` and the version's `hw_init`, then
 * `psmouse_initialize` and the enable. */
static int took_reconnect(void)
{
    /* The reset and defaults, the trial identification and the CS19
     * check's 0xE1, which the reconnect does not send. */
    int skip = 3 + 19 + 1;

    n_want = 0;
    expect_n(bringup_took + skip, n_bringup_took - skip);
    return took_expected();
}

/** A version 3 DualPoint reconnected after twelve bytes out of form in a
 * row and after its announcement, keeping its event devices, as Linux's
 * psmouse and `alps_reconnect` do; and probed anew when no ALPS pad
 * answers in its place. */
static void reconnect_rows(void)
{
    static const uint8_t one[6] = {0x8F, 0x3E, 0x2B, 0x08, 0x2C, 0x32};
    struct evdev *pad_dev, *stick_dev;
    int i;

    v3_setup();
    k_memcpy(power_on_regs, regs, sizeof(regs));
    CHECK("a version 3 DualPoint is brought up", bring_up() == 0 && alps == 1);
    k_memcpy(bringup_took, took, sizeof(took));
    n_bringup_took = n_took;
    pad_dev = pad;
    stick_dev = stick;
    for (i = 0; i < 4; i++) {
        irq_byte(0x00);
    }
    CHECK("four first bytes out of form are four bad packets",
          bad_in_a_row == 4 && state == STATE_ACTIVE && !reconnect_wanted);
    send(one, 6);
    CHECK("a whole packet ends the count", bad_in_a_row == 0);
    for (i = 0; i < 11; i++) {
        irq_byte(0x00);
    }
    CHECK("eleven bad bytes, two lost packets' worth, leave the port as it is",
          bad_in_a_row == 11 && state == STATE_ACTIVE && !reconnect_wanted);
    irq_byte(0x00);
    CHECK("the twelfth, Linux's resetafter of twice the packet's size, stops the port and "
          "asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT);
    reconnect_pad();
    CHECK("the pad is reconnected with Linux's commands, in Linux's order", took_reconnect());
    CHECK("it is in absolute mode again and reports",
          regs[0x0004] == 0x06 && !cmd_mode && reporting && state == STATE_ACTIVE);
    CHECK("it keeps its event devices", pad == pad_dev && stick == stick_dev && alps == 1);
    send(one, 6);
    CHECK("and its packets are read", pktcnt == 0 && bad_in_a_row == 0);

    irq_byte(0xAA);
    irq_byte(0x00);
    CHECK("its announcement 0xAA 0x00 asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT);
    reconnect_pad();
    CHECK("which sends the same commands", took_reconnect() && pad == pad_dev);

    for (i = 0; i < 12; i++) {
        irq_byte(0x00);
    }
    dev_kind = KIND_PLAIN;
    ps2mouse_reconnect();
    CHECK("a mouse that answers in the pad's place is probed anew",
          alps == 0 && pad == 0 && stick == 0 && mouse_dev != NULL && reporting &&
              state == STATE_ACTIVE);
}

int main(void)
{
    table_rows();
    v1_v2_setup_rows();
    v3_setup_rows();
    rushmore_v4_setup_rows();
    v5_v6_setup_rows();
    v7_setup_rows();
    v8_setup_rows();
    left_alone_rows();
    probe_rows();
    capability_rows();
    v1_packet_rows();
    v2_packet_rows();
    interleaved_rows();
    v3_packet_rows();
    rushmore_packet_rows();
    v4_packet_rows();
    v5_packet_rows();
    v6_packet_rows();
    v7_packet_rows();
    ss4_packet_rows();
    reconnect_rows();
    model_reset(KIND_PLAIN);
    if (fails == 0) {
        printf("alps-test: pass\n");
    }
    return fails ? 1 : 0;
}
