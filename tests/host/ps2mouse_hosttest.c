/**
 * The PS/2 mouse's bring-up, its IRQ 12 handler and its commands, driven on
 * the host against a model of the 8042 and the mouse behind it
 * (sys/arch/x86/ps2mouse.c).
 *
 * The file is included whole so that the static functions and the packet
 * state are reachable, with the 8042's waits of i8042.c. The model's output
 * buffer is a queue of bytes, each with its status: the status register reads
 * as the first one's, and a read of the data port is counted and takes it. A
 * status with the output buffer full (bit 0) and bit 5 clear marks a keyboard
 * byte, which the handler must leave unread for the keyboard's IRQ 1 handler,
 * with the packet as it was. With bit 5 set the byte is the mouse's: the
 * handler reads it, and three make a packet. A command to the mouse takes as
 * its reply only a byte marked as the mouse's, and drops a keyboard byte that
 * comes first.
 *
 * Every port write is kept in order. A write to port 0x64 is a command to the
 * controller: 0xA8 and 0xA7 clear and set the configuration byte's
 * aux-clock-disable bit, 0x20 queues the configuration byte, and 0x60 and
 * 0xD4 send the next write to port 0x60 to the configuration byte and to the
 * mouse. The mouse answers only while its clock is enabled: an ACK to each
 * command and argument, 0xAA and its ID 0x00 after the ACK of a reset, its
 * ID after the ACK of 0xF2, and reporting on after an ACK of 0xF4. A row can
 * give it a wheel: then, as QEMU's mouse, it answers ID 3 after the sample
 * rates 200, 100 and 80, and an Explorer ID 4 after 200, 200 and 80, until
 * it is reset. A row can make the mouse lose its answer to one command, or
 * ask for that command again with 0xFE, once or twice, and can make the
 * controller lose the configuration byte, keep its input buffer full, keep
 * it full from a WRITE_CONFIG on, or keep its output buffer full for good;
 * a row can have the controller answer late, as interrupts come on again,
 * when the keyboard's IRQ 1 and LED tick run unless `kbd_stop` stopped the
 * keyboard first. The rows of the bring-up check the writes in order, the
 * state the controller and the mouse are left in, the protocol found and the
 * event device registered for it, the vector of the handler, and that
 * interrupts are off for every port access and on again at every return.
 * The handler's rows read the packets it reports, of three bytes and of each
 * wheel's four. The rows of the waits count the reads of the status and the
 * waits asked of `k_udelay`, and the time of the waits for the mouse's reply,
 * whose clock (`host_clock_us`) a row may stop, whose delays may measure
 * only a part of each step (`host_delay_cut`), and whose mouse may answer
 * only after a time (`model_late_us`, `model_bat_late_us`). The probes for
 * a Synaptics, an ALPS and an Elantech touchpad, which synaptics_hosttest.c,
 * alps_hosttest.c and elantech_hosttest.c test against touchpads, find none
 * here, and the model answers neither a ThinkingMouse's rates nor a Genius
 * mouse's status request as those mice do, so their probes find none either.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2mouse.c"

int printf(const char *format, ...);
int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("ps2mouse-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
    } while (0)

/* The two statuses of a byte waiting in the output buffer. */
#define KEYBOARD_BYTE PS2_STATUS_OUTPUT_FULL
#define MOUSE_BYTE    (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)

/* The configuration byte the firmware leaves: the keyboard's interrupt, the
 * system flag, translation, and the aux clock disabled. A bring-up sets the
 * mouse's interrupt and enables its clock. */
#define FIRMWARE_CONFIG 0x65
#define MOUSE_CONFIG    0x47

/* The model 8042: the bytes waiting in its output buffer, first at
 * `model_head`, the reads of its ports, and every port write in order. */
#define MODEL_BYTES  16
#define MODEL_WRITES 160
static uint8_t model_status[MODEL_BYTES];
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;
static int status_reads;
static int data_reads;
static int port_writes;
static uint16_t write_port[MODEL_WRITES];
static uint8_t write_value[MODEL_WRITES];

/* The controller: its configuration byte; what the next write to port 0x60
 * is for, 0 for the keyboard or the command that named it; and the faults a
 * row gives it. */
static uint8_t ctl_config;
static uint8_t ctl_next;
static int ctl_loses_config;
static int ctl_input_stuck;
static int ctl_stalls_write_config;
static int ctl_output_stuck;

/* The keyboard on the controller's first port: whether `kbd_stop` stopped
 * it and whether interrupts were on then, and whether the controller
 * answers late and the keyboard wakes as interrupts come on again
 * (`keyboard_wakes`). */
static int kbd_stopped;
static int kbd_stopped_with_irq_on;
static int kbd_wakes;

/* The mouse: whether one is there, a command it never answers and one it
 * first answers with MOUSE_RESEND (-1 for none), how many times it does,
 * and whether it reports. */
static int mouse_present;
static int mouse_silent_to;
static int mouse_resend_to;
static int mouse_resends;
static int mouse_reporting;

/* The waits asked of `k_udelay`, and their microseconds. */
static int udelays;
static uint32_t udelay_us;

/* The time of the waits for the mouse's reply before which the output
 * buffer shows none of its bytes, as a mouse that answers late, and before
 * which it shows no 0xAA, as a mouse whose self-test takes that long. */
static uint32_t model_late_us;
static uint32_t model_bat_late_us;

/* The mouse's wheel: the highest ID it answers, 0, ID_IMPS or ID_IMEX; the
 * ID it answers now; the command whose argument the next byte is, 0 for
 * none; and how far into a knock the rates have gone, as QEMU counts. */
static int mouse_wheel;
static int mouse_id;
static uint8_t mouse_arg_for;
static int mouse_knock;

/* Interrupts, as `arch_irq_disable` and `arch_irq_enable` leave them, and the
 * port accesses made while they were on. */
static int irq_on = 1;
static int ports_with_irq_on;

/* The handlers registered and the warnings logged. */
static int registrations;
static uint8_t registered_vector;
static isr_handler_t registered_handler;
static int registered_with_irq_on;
static int warnings;
static const char *last_warning;

/* The event device registered, and the packets the handler reported. */
static int mouse_registrations;
static const char *registered_name;
static const char *registered_phys;
static uint16_t registered_bus;
static uint16_t registered_vendor;
static uint16_t registered_product;
static unsigned int registered_caps;
static int reports;
static unsigned int reported_buttons;
static int reported_dx;
static int reported_dy;
static int reported_wheel;
static int reported_hwheel;

/** Put `data` in the output buffer behind what waits there, with `status`. */
static void model_put(uint8_t status, uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("ps2mouse-test: fail: the model's output buffer overflowed\n");
        fails++;
        return;
    }
    model_status[model_head + model_count] = status;
    model_data[model_head + model_count] = data;
    model_count++;
}

/** Empty the output buffer. */
static void model_clear(void)
{
    model_head = 0;
    model_count = 0;
}

/** Put the controller and the mouse as the firmware leaves them, with no
 * mouse answering, no fault, and the counts at zero. */
static void model_reset(void)
{
    model_clear();
    status_reads = 0;
    data_reads = 0;
    port_writes = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    ctl_loses_config = 0;
    ctl_input_stuck = 0;
    ctl_stalls_write_config = 0;
    ctl_output_stuck = 0;
    kbd_stopped = 0;
    kbd_stopped_with_irq_on = 0;
    kbd_wakes = 0;
    mouse_present = 0;
    mouse_silent_to = -1;
    mouse_resend_to = -1;
    mouse_resends = 1;
    mouse_reporting = 0;
    udelays = 0;
    udelay_us = 0;
    model_late_us = 0;
    model_bat_late_us = 0;
    host_clock_us = 0; /* ps2mouse.c's, the reply wait's clock */
    host_slept_us = 0;
    host_clock_still = 0;
    host_delay_cut = 0;
    mouse_wheel = 0;
    mouse_id = 0;
    mouse_arg_for = 0;
    mouse_knock = 0;
    mouse_registrations = 0;
    registered_name = 0;
    setup_failed = 0; /* i8042.c's, as at boot */
    registered_product = 0;
    irq_on = 1;
    ports_with_irq_on = 0;
    registrations = 0;
    registered_vector = 0;
    registered_handler = 0;
    registered_with_irq_on = 0;
    warnings = 0;
    last_warning = 0;
}

/** Take the sample rate `rate`, which moves a knock on as QEMU's mouse
 * does: 200 starts one, 100 or 200 then chooses it, and 80 ends it. */
static void mouse_rate(uint8_t rate)
{
    switch (mouse_knock) {
    case 0:
        mouse_knock = rate == 200 ? 1 : 0;
        break;
    case 1:
        mouse_knock = rate == 100 ? 2 : rate == 200 ? 3 : 0;
        break;
    case 2:
        if (rate == 80 && mouse_wheel >= ID_IMPS) {
            mouse_id = ID_IMPS;
        }
        mouse_knock = 0;
        break;
    default:
        if (rate == 80 && mouse_wheel >= ID_IMEX) {
            mouse_id = ID_IMEX;
        }
        mouse_knock = 0;
        break;
    }
}

/** Take `cmd`, a byte the controller sent on to the mouse. */
static void mouse_take(uint8_t cmd)
{
    if (!mouse_present || (ctl_config & PS2_CONFIG_MOUSE_CLK) != 0 || cmd == mouse_silent_to) {
        return;
    }
    if (cmd == mouse_resend_to) {
        if (--mouse_resends == 0) {
            mouse_resend_to = -1;
        }
        model_put(MOUSE_BYTE, MOUSE_RESEND);
        return;
    }
    model_put(MOUSE_BYTE, MOUSE_ACK);
    if (mouse_arg_for) {
        if (mouse_arg_for == MOUSE_CMD_SET_RATE) {
            mouse_rate(cmd);
        }
        mouse_arg_for = 0;
        return;
    }
    if (cmd == MOUSE_CMD_SET_RATE || cmd == MOUSE_CMD_SET_RES) {
        mouse_arg_for = cmd;
    } else if (cmd == MOUSE_CMD_GET_ID) {
        model_put(MOUSE_BYTE, (uint8_t)mouse_id);
    } else if (cmd == MOUSE_CMD_RESET) {
        mouse_reporting = 0;
        mouse_id = 0;
        mouse_knock = 0;
        model_put(MOUSE_BYTE, 0xAA);
        model_put(MOUSE_BYTE, 0x00);
    } else if (cmd == MOUSE_CMD_SET_DEFAULTS || cmd == MOUSE_CMD_DISABLE) {
        mouse_reporting = 0;
    } else if (cmd == MOUSE_CMD_ENABLE) {
        mouse_reporting = 1;
    }
}

uint8_t inb(uint16_t port)
{
    uint8_t status = 0;
    uint8_t data;

    if (irq_on) {
        ports_with_irq_on++;
    }
    if (port == PS2_STATUS_PORT) {
        status_reads++;
        if (ctl_output_stuck) {
            status = PS2_STATUS_OUTPUT_FULL;
        } else if (model_count > 0 && host_slept_us >= model_late_us &&
                   (model_data[model_head] != 0xAA || host_slept_us >= model_bat_late_us)) {
            status = model_status[model_head];
        }
        if (ctl_input_stuck) {
            status |= PS2_STATUS_INPUT_FULL;
        }
        return status;
    }
    if (port == PS2_DATA_PORT) {
        data_reads++;
        if (ctl_output_stuck) {
            return 0xFA;
        }
        if (model_count == 0) {
            return 0;
        }
        data = model_data[model_head];
        model_head++;
        model_count--;
        return data;
    }
    return 0xFF;
}

void outb(uint16_t port, uint8_t val)
{
    uint8_t next = ctl_next;

    if (irq_on) {
        ports_with_irq_on++;
    }
    if (port_writes < MODEL_WRITES) {
        write_port[port_writes] = port;
        write_value[port_writes] = val;
    }
    port_writes++;
    if (port == PS2_CMD_PORT) {
        ctl_next = 0;
        if (val == PS2_CMD_ENABLE_MOUSE) {
            ctl_config &= ~PS2_CONFIG_MOUSE_CLK;
        } else if (val == PS2_CMD_DISABLE_MOUSE) {
            ctl_config |= PS2_CONFIG_MOUSE_CLK;
        } else if (val == PS2_CMD_READ_CONFIG) {
            if (!ctl_loses_config) {
                model_put(KEYBOARD_BYTE, ctl_config);
            }
        } else if (val == PS2_CMD_WRITE_CONFIG || val == PS2_CMD_WRITE_MOUSE) {
            ctl_next = val;
            if (val == PS2_CMD_WRITE_CONFIG && ctl_stalls_write_config) {
                ctl_input_stuck = 1;
            }
        }
    } else if (port == PS2_DATA_PORT) {
        ctl_next = 0;
        if (next == PS2_CMD_WRITE_CONFIG) {
            ctl_config = val;
        } else if (next == PS2_CMD_WRITE_MOUSE) {
            mouse_take(val);
        }
    }
}

void arch_irq_disable(void)
{
    irq_on = 0;
}

/**
 * The controller answers late, as interrupts come on again: the input
 * buffer it kept full empties, with a WRITE_CONFIG still taking the next
 * byte, and the configuration byte it lost arrives. Then IRQ 1 and the LED
 * tick run, unless `kbd_stop` has stopped the keyboard, as kbd_irq checks
 * kbd.off and ps2kbd_tick leds.ready: the handler reads the keyboard's byte
 * waiting, and the tick writes the LED command, 0xED.
 */
static void keyboard_wakes(void)
{
    if (ctl_loses_config) {
        ctl_loses_config = 0;
        model_put(KEYBOARD_BYTE, ctl_config);
    }
    ctl_input_stuck = 0;
    if (kbd_stopped) {
        return;
    }
    if (model_count > 0 && model_status[model_head] == KEYBOARD_BYTE) {
        (void)inb(PS2_DATA_PORT);
    }
    outb(PS2_DATA_PORT, 0xED);
}

void arch_irq_enable(void)
{
    irq_on = 1;
    if (kbd_wakes) {
        kbd_wakes = 0;
        keyboard_wakes();
    }
}

void kbd_stop(void)
{
    kbd_stopped = 1;
    kbd_stopped_with_irq_on = irq_on;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)name;
    registrations++;
    registered_vector = vector;
    registered_handler = handler;
    registered_with_irq_on = irq_on;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
        last_warning = format;
    }
}

/** The relative-mode probe sees the same modeled device. */
int synaptics_init_relative(void)
{
    return synaptics_init();
}

/** No Synaptics touchpad answers. */
int synaptics_init(void)
{
    return 0;
}

/** No touchpad's bytes come, since `synaptics_init` found none. */
int synaptics_byte(uint8_t data)
{
    (void)data;
    return SYNAPTICS_GOOD;
}

/* The rest of synaptics.h and trackpoint.h, which a bring-up without a
 * touchpad never calls; trackpoint_hosttest.c tests them. */
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

unsigned int synaptics_relative_caps(void)
{
    return 0;
}

uint16_t synaptics_model(void)
{
    return 0;
}

int synaptics_sysfs_add(int dir)
{
    (void)dir;
    return 0;
}

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

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return 0;
}

unsigned int synaptics_set_rate(unsigned int rate)
{
    return rate;
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

void evdev_set_capability(struct evdev *dev, uint16_t type, uint16_t code)
{
    (void)dev;
    (void)type;
    (void)code;
}

void evdev_set_prop(struct evdev *dev, unsigned int prop)
{
    (void)dev;
    (void)prop;
}

void evdev_set_version(struct evdev *dev, uint16_t version)
{
    (void)dev;
    (void)version;
}

void evdev_unregister(struct evdev *dev)
{
    (void)dev;
}

/** The 10 ms a pass-through probe waits, which no row reaches. */
void k_udelay(uint32_t usec)
{
    udelays++;
    udelay_us += usec;
}

/* The ticks of the kernel's clock, which `ps2mouse_msleep` moves. */
volatile uint64_t kernel_ticks;

/** Interrupts off around the auxiliary port's two-byte write, as the
 * kernel's `kirq_save` turns them off, and back as they were. */
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

/** No Elantech touchpad answers. */
int elantech_init(void)
{
    return 0;
}

/** No ALPS touchpad answers. */
int alps_init(void)
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

/** Reserve an event device; its pointer is never followed by the model. */
struct evdev *evdev_reserve(void)
{
    return (struct evdev *)&mouse_registrations;
}

/** Record the identity of the registered device. */
struct evdev *evdev_register_reserved(struct evdev *dev, const char *name, uint16_t bustype,
                                      uint16_t vendor, uint16_t product, const uint8_t *keys)
{
    (void)dev;
    (void)keys;
    mouse_registrations++;
    registered_name = name;
    registered_bus = bustype;
    registered_vendor = vendor;
    registered_product = product;
    return (struct evdev *)&mouse_registrations;
}

/** Record the mouse's path and capabilities. */
void mouse_configure(struct evdev *dev, const char *phys, unsigned int caps)
{
    (void)dev;
    registered_phys = phys;
    registered_caps = caps;
}

/** The command lock never has another caller in this model. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Record the packet reported, and count it. */
void mouse_report(struct evdev *dev, unsigned int buttons, int dx, int dy, int wheel, int hwheel)
{
    (void)dev;
    reports++;
    reported_buttons = buttons;
    reported_dx = dx;
    reported_dy = dy;
    reported_wheel = wheel;
    reported_hwheel = hwheel;
}

/** Whether the port writes so far are the `n` of `ports` and `values`. */
static int writes_are(const uint16_t *ports, const uint8_t *values, int n)
{
    int i;

    if (port_writes != n || n > MODEL_WRITES) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (write_port[i] != ports[i] || write_value[i] != values[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether `last_warning` is `text`. */
static int warned(const char *text)
{
    return last_warning != 0 && strcmp(last_warning, text) == 0;
}

/** Run one IRQ 12 with `status` in the status register and `data` waiting,
 * and return the number of times it read the data port. */
static int interrupt(uint8_t status, uint8_t data)
{
    int before = data_reads;

    model_clear();
    model_put(status, data);
    mouse_irq_handler(0);
    model_clear();
    return data_reads - before;
}

/** The IRQ 12 handler: whose byte it reads, and the packets it makes. */
static void handler_rows(void)
{
    model_reset();

    /* A keyboard byte when no packet is under way. */
    CHECK("keyboard byte leaves the data port unread", interrupt(KEYBOARD_BYTE, 0x1C) == 0);
    CHECK("keyboard byte starts no packet", packet_index == 0);

    /* The first byte of a packet: left button, X and Y signs set. */
    CHECK("mouse byte is read", interrupt(MOUSE_BYTE, 0x39) == 1);
    CHECK("mouse byte advances the packet", packet_index == 1);
    CHECK("mouse byte is byte 0", packet_bytes[0] == 0x39);

    /* A keyboard byte in the middle of the packet. */
    CHECK("keyboard byte mid-packet leaves the data port unread",
          interrupt(KEYBOARD_BYTE, 0x9C) == 0);
    CHECK("keyboard byte mid-packet leaves the packet", packet_index == 1);
    CHECK("keyboard byte mid-packet leaves byte 0", packet_bytes[0] == 0x39);

    /* An auxiliary mark with the output buffer empty. */
    CHECK("empty buffer leaves the data port unread", interrupt(PS2_STATUS_MOUSE_DATA, 0xAA) == 0);
    CHECK("empty buffer leaves the packet", packet_index == 1);

    /* The rest of the packet: X is -2 and Y, positive away from the user,
     * is -3. */
    reports = 0;
    CHECK("byte 1 is read", interrupt(MOUSE_BYTE, 0xFE) == 1);
    CHECK("byte 2 is read", interrupt(MOUSE_BYTE, 0xFD) == 1);
    CHECK("three mouse bytes make one packet", reports == 1);
    CHECK("the packet starts the next at byte 0", packet_index == 0);
    CHECK("the packet has the left button", reported_buttons == MOUSE_LEFT);
    CHECK("the packet's dx is -2", reported_dx == -2);
    CHECK("the packet's dy is 3, toward the user as REL_Y has it", reported_dy == 3);
    CHECK("a plain mouse's packet turns no wheel", reported_wheel == 0 && reported_hwheel == 0);

    /* A byte out of step with the stream, in the place of byte 0. */
    CHECK("a byte without bit 3 is read", interrupt(MOUSE_BYTE, 0x02) == 1);
    CHECK("a first byte without bit 3 is dropped", packet_index == 0);

    /* A packet whose X movement overflowed, read as Linux's
     * psmouse_report_standard_motion reads it, its overflow bit not looked
     * at. */
    interrupt(MOUSE_BYTE, 0x49);
    interrupt(MOUSE_BYTE, 0x10);
    interrupt(MOUSE_BYTE, 0x10);
    CHECK("a packet with an overflow bit is reported as Linux reports it",
          reports == 2 && reported_buttons == MOUSE_LEFT && reported_dx == 16 &&
              reported_dy == -16);
    CHECK("it starts the next at byte 0", packet_index == 0);

    /* Bytes of 0 with their sign bits set, which Linux reads as no
     * movement. */
    interrupt(MOUSE_BYTE, 0x38);
    interrupt(MOUSE_BYTE, 0x00);
    interrupt(MOUSE_BYTE, 0x00);
    CHECK("a movement byte of 0 is no movement whatever its sign bit",
          reports == 3 && reported_buttons == 0 && reported_dx == 0 && reported_dy == 0);

    CHECK("the handler writes no port", port_writes == 0);
}

/** Feed the four bytes of a wheel's packet to the handler. */
static void packet4(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    interrupt(MOUSE_BYTE, b0);
    interrupt(MOUSE_BYTE, b1);
    interrupt(MOUSE_BYTE, b2);
    interrupt(MOUSE_BYTE, b3);
}

/** The packets of the IntelliMouse and the Explorer, read as Linux's
 * psmouse reads them. */
static void wheel_rows(void)
{
    model_reset();
    protocol = PROTO_IMPS;
    packet_size = 4;
    packet_index = 0;
    reports = 0;
    interrupt(MOUSE_BYTE, 0x08);
    interrupt(MOUSE_BYTE, 0x01);
    interrupt(MOUSE_BYTE, 0x00);
    CHECK("an IntelliMouse's packet waits for its fourth byte", reports == 0 && packet_index == 3);
    interrupt(MOUSE_BYTE, 0xFF);
    CHECK("the fourth byte ends it", reports == 1 && packet_index == 0);
    CHECK("its wheel's -1 is a turn up, +1", reported_wheel == 1 && reported_dx == 1);
    packet4(0x0C, 0x00, 0x00, 0x02);
    CHECK("a turn down of 2 and the middle button",
          reported_wheel == -2 && reported_buttons == MOUSE_MIDDLE);

    protocol = PROTO_IMEX;
    extra_buttons = 0;
    packet4(0x08, 0x00, 0x00, 0x0F);
    CHECK("an Explorer's 4-bit -1 is a turn up", reported_wheel == 1 && reported_buttons == 0);
    packet4(0x08, 0x00, 0x00, 0x31);
    CHECK("its side and extra buttons, with a turn down",
          reported_wheel == -1 && reported_buttons == (MOUSE_BUTTON_SIDE | MOUSE_BUTTON_EXTRA));
    packet4(0x08, 0x00, 0x00, 0x85);
    CHECK("a 6-bit turn of an Explorer 4.0 keeps the buttons held",
          reported_wheel == -5 && reported_buttons == (MOUSE_BUTTON_SIDE | MOUSE_BUTTON_EXTRA));
    packet4(0x08, 0x00, 0x00, 0x7F);
    CHECK("its horizontal turn, 0x40 and a 6-bit -1, is to the right",
          reported_hwheel == 1 && reported_wheel == 0);
    packet4(0x08, 0x00, 0x00, 0x00);
    CHECK("a packet of 0x00 lets the side buttons go", reported_buttons == 0);
    CHECK("the wheels' packets write no port", port_writes == 0);
    protocol = PROTO_PS2;
    packet_size = 3;
    packet_index = 0;
}

/** The reads of the status a wait for the mouse's reply that never comes
 * makes in `limit_us`, by `reply_step`, and whether its steps add up to
 * `limit_us`, each of PS2_POLL_US to a tick. */
static int reply_reads(uint32_t limit_us, int *exact)
{
    uint32_t waited = 0, step;
    int reads = 1;

    *exact = 1;
    while (waited < limit_us) {
        step = reply_step(waited, limit_us);
        if (step > TICK_US || (step < PS2_POLL_US && step != limit_us - waited)) {
            *exact = 0;
        }
        waited += step;
        reads++;
    }
    if (waited != limit_us) {
        *exact = 0;
    }
    return reads;
}

/** The waits' bounds: the controller's by reads and by time, as Linux's
 * i8042_wait_write, and the mouse's reply's by Linux's times, in a few
 * reads of the status. */
static void wait_rows(void)
{
    int exact, reads;

    model_reset();
    ctl_input_stuck = 1;
    CHECK("a full input buffer ends the controller's wait", ps2_wait_input() == -1);
    CHECK("after PS2_TIMEOUT waits of PS2_POLL_US, between PS2_TIMEOUT + 1 reads",
          status_reads == PS2_TIMEOUT + 1 && udelays == PS2_TIMEOUT &&
              udelay_us == PS2_TIMEOUT_US && PS2_TIMEOUT_US == 500000u);

    reads = reply_reads(REPLY_ACK_US, &exact);
    CHECK("a wait for an ACK that never comes reads the status 89 times in 200 ms",
          reads == 89 && exact && REPLY_ACK_US == 200000u);
    model_reset();
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    mouse_silent_to = MOUSE_CMD_ENABLE;
    CHECK("an unanswered command fails", mouse_send_command(MOUSE_CMD_ENABLE) == -1);
    CHECK("after those reads and the two of the controller's waits, with no wait of the "
          "controller's",
          status_reads == 2 + reads && udelays == 0);

    reads = reply_reads(REPLY_RESET_ACK_US, &exact);
    model_reset();
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    mouse_silent_to = MOUSE_CMD_RESET;
    CHECK("an unanswered reset fails", mouse_send_command(MOUSE_CMD_RESET) == -1);
    CHECK("after its wait of a second", status_reads == 2 + reads && exact && reads == 289);

    model_reset();
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    CHECK("an answered reset reads its 0xAA and ID",
          mouse_send_command(MOUSE_CMD_RESET) == MOUSE_ACK && ps2mouse_receive() == 0xAA &&
              ps2mouse_receive() == 0x00);
    reads = reply_reads(REPLY_RESET_ID_US, &exact);
    status_reads = 0;
    CHECK("after which a byte more is waited for 100 ms",
          ps2mouse_receive() == -1 && status_reads == reads && exact);
    reads = reply_reads(REPLY_BYTE_US, &exact);
    status_reads = 0;
    CHECK("and after the next ACK, half a second",
          mouse_send_command(MOUSE_CMD_GET_ID) == MOUSE_ACK && ps2mouse_receive() == 0 &&
              ps2mouse_receive() == -1 && status_reads == 2 + 1 + 1 + reads && exact);
}

/** The wait for the mouse's reply on a clock that stands still, as a TSC
 * that pauses: the delays waited measure the wait instead, which neither
 * ends sooner than its time nor goes on for good. */
static void still_clock_rows(void)
{
    int exact, reads;

    reads = reply_reads(REPLY_RESET_ACK_US, &exact);
    model_reset();
    host_clock_still = 1;
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    model_late_us = 750000u;
    CHECK("with the clock stopped, a reset answered after 750 ms reads its ACK, 0xAA and ID",
          mouse_send_command(MOUSE_CMD_RESET) == MOUSE_ACK && ps2mouse_receive() == 0xAA &&
              ps2mouse_receive() == 0x00 && host_slept_us >= 750000u &&
              host_slept_us < REPLY_RESET_ACK_US && host_clock_us == 0);
    model_reset();
    host_clock_still = 1;
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    mouse_silent_to = MOUSE_CMD_RESET;
    CHECK("and an unanswered reset fails after its second, in the reads of a running clock",
          mouse_send_command(MOUSE_CMD_RESET) == -1 && host_slept_us == REPLY_RESET_ACK_US &&
              status_reads == 2 + reads);

    /* A stopped clock whose delays wait and measure a quarter of each step,
     * as on a stopped TSC with no other counter: the wait counts what the
     * delays measured, not what they were asked, so neither allowance ends
     * before its time. */
    model_reset();
    host_clock_still = 1;
    host_delay_cut = 4;
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    model_late_us = 750000u;
    model_bat_late_us = 1750000u;
    CHECK("with delays short of their steps, an ACK after 750 ms and a self-test a second later "
          "are read",
          mouse_send_command(MOUSE_CMD_RESET) == MOUSE_ACK && ps2mouse_receive() == 0xAA &&
              ps2mouse_receive() == 0x00 && host_slept_us >= 1750000u &&
              host_slept_us < 750000u + REPLY_RESET_BAT_US && host_clock_us == 0);
    model_reset();
    host_clock_still = 1;
    host_delay_cut = 4;
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    mouse_silent_to = MOUSE_CMD_RESET;
    CHECK("and an unanswered reset fails after a second of what they measured",
          mouse_send_command(MOUSE_CMD_RESET) == -1 && host_slept_us == REPLY_RESET_ACK_US);
}

/** The mouse's commands, with keys pressed while they wait for the reply. */
static void command_rows(void)
{
    /* A key pressed before the mouse acknowledges a command. */
    model_reset();
    model_put(KEYBOARD_BYTE, 0x1C);
    model_put(MOUSE_BYTE, MOUSE_ACK);
    CHECK("a key before the ACK is not the reply",
          mouse_send_command(MOUSE_CMD_ENABLE) == MOUSE_ACK);
    CHECK("the key and the ACK are both read", model_count == 0);
    CHECK("the command goes to the mouse", port_writes == 2 && write_port[0] == PS2_CMD_PORT &&
                                               write_value[0] == PS2_CMD_WRITE_MOUSE &&
                                               write_port[1] == PS2_DATA_PORT &&
                                               write_value[1] == MOUSE_CMD_ENABLE);

    /* A key and nothing from the mouse: one try, timed out. */
    model_reset();
    model_put(KEYBOARD_BYTE, 0x1C);
    CHECK("a key alone is no reply", mouse_send_command(MOUSE_CMD_ENABLE) == -1);
    CHECK("the command is not sent again after the timeout", port_writes == 2);

    /* A mouse that asks for the command again, and again. */
    model_reset();
    mouse_present = 1;
    ctl_config = MOUSE_CONFIG;
    mouse_resend_to = MOUSE_CMD_ENABLE;
    mouse_resends = 2;
    CHECK("a second request to send again fails the command",
          mouse_send_command(MOUSE_CMD_ENABLE) == -1);
    CHECK("which was sent twice", port_writes == 4 && !mouse_reporting);

    wait_rows();
    still_clock_rows();

    /* Keys pressed between the bytes of the reset's reply. */
    model_reset();
    model_put(MOUSE_BYTE, MOUSE_ACK);
    model_put(KEYBOARD_BYTE, 0x1E);
    model_put(MOUSE_BYTE, 0xAA);
    model_put(KEYBOARD_BYTE, 0x9E);
    model_put(MOUSE_BYTE, 0x00);
    CHECK("the reset is answered", mouse_reset() == 0);
    CHECK("the reset's reply leaves no byte waiting", model_count == 0);
}

#define C PS2_CMD_PORT
#define D PS2_DATA_PORT

/* The writes a row expects, built with the `expect_*` calls below. */
static uint16_t expect_port[MODEL_WRITES];
static uint8_t expect_value[MODEL_WRITES];
static int expected;

/** Expect `value` written to `port`. */
static void expect(uint16_t port, uint8_t value)
{
    if (expected < MODEL_WRITES) {
        expect_port[expected] = port;
        expect_value[expected] = value;
    }
    expected++;
}

/** Expect the byte `b` sent to the mouse, `n` times. */
static void expect_mouse(uint8_t b, int n)
{
    while (n-- > 0) {
        expect(C, 0xD4);
        expect(D, b);
    }
}

/** Expect the controller's part of a bring-up: the aux port enabled and
 * the configuration byte read and written. */
static void expect_controller(void)
{
    expected = 0;
    expect(C, 0xA8);
    expect(C, 0x20);
    expect(C, 0x60);
    expect(D, MOUSE_CONFIG);
}

/** Expect the knock of the rates `a`, `b` and `c`, and the ID read. */
static void expect_knock(uint8_t a, uint8_t b, uint8_t c)
{
    expect_mouse(0xF3, 1);
    expect_mouse(a, 1);
    expect_mouse(0xF3, 1);
    expect_mouse(b, 1);
    expect_mouse(0xF3, 1);
    expect_mouse(c, 1);
    expect_mouse(0xF2, 1);
}

/** Expect the probes of psmouse's order that come before the knocks: a
 * Kensington ThinkingMouse's rates and ID, then a Genius mouse's
 * resolution, three 1:1 scalings and status request. */
static void expect_extensions(void)
{
    static const uint8_t thinking[9] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
    int i;

    expect_mouse(0xF3, 1);
    expect_mouse(10, 1);
    expect_mouse(0xE8, 1);
    expect_mouse(0, 1);
    for (i = 0; i < 9; i++) {
        expect_mouse(0xF3, 1);
        expect_mouse(thinking[i], 1);
    }
    expect_mouse(0xF2, 1);
    expect_mouse(0xE8, 1);
    expect_mouse(3, 1);
    expect_mouse(0xE6, 3);
    expect_mouse(0xE9, 1);
}

/** Expect a plain mouse's protocol found and its settings made: the
 * probes before the knocks, the IntelliMouse's knock unanswered, then rate
 * 100, resolution 3 and 1:1. */
static void expect_plain(void)
{
    expect_extensions();
    expect_knock(200, 100, 80);
    expect_mouse(0xF3, 1);
    expect_mouse(100, 1);
    expect_mouse(0xE8, 1);
    expect_mouse(3, 1);
    expect_mouse(0xE6, 1);
}

/** Whether the port writes so far are the ones expected. */
static int writes_expected(void)
{
    return writes_are(expect_port, expect_value, expected);
}

/** `init_mouse` against a working controller and mouse, and against each
 * fault of the controller. */
static void bringup_controller_rows(void)
{
    static const uint16_t lost_config_ports[] = {C, C};
    static const uint8_t lost_config_values[] = {0xA8, 0x20};

    /* A working controller, with a key and a mouse byte the firmware left. */
    model_reset();
    mouse_present = 1;
    model_put(KEYBOARD_BYTE, 0x1C);
    model_put(MOUSE_BYTE, 0x08);
    CHECK("the bring-up on a working controller succeeds", init_mouse() == 0);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_plain();
    expect_mouse(0xF4, 1);
    CHECK("the controller is written the bring-up's sequence", writes_expected());
    CHECK("the configuration byte enables IRQ 12 and the aux clock and keeps the rest",
          ctl_config == MOUSE_CONFIG);
    CHECK("the mouse reports", mouse_reporting);
    CHECK("every byte of the bring-up is read", model_count == 0);
    CHECK("the handler is registered once",
          registrations == 1 && registered_handler == mouse_irq_handler);
    CHECK("the handler is on vector 44", registered_vector == 44);
    CHECK("the handler is registered with interrupts off", !registered_with_irq_on);
    CHECK("the bring-up touches no port with interrupts on", ports_with_irq_on == 0);
    CHECK("the bring-up turns interrupts on again", irq_on);
    CHECK("a working controller logs no warning", warnings == 0);
    CHECK("a working controller leaves the keyboard running", !kbd_stopped);
    CHECK("a plain mouse is PS/2 Generic Mouse, isa0060/serio1/input0",
          mouse_registrations == 1 && strcmp(registered_name, "PS/2 Generic Mouse") == 0 &&
              strcmp(registered_phys, "isa0060/serio1/input0") == 0);
    CHECK("on the 8042's bus, vendor 2 and product PSMOUSE_PS2",
          registered_bus == EVDEV_BUS_I8042 && registered_vendor == 2 &&
              registered_product == PROTO_PS2);
    CHECK("a pointer with no wheel", registered_caps == MOUSE_POINTER);
    CHECK("its packets are three bytes", packet_size == 3);

    /* An output buffer that is full for good. */
    model_reset();
    mouse_present = 1;
    ctl_output_stuck = 1;
    CHECK("a stuck output buffer fails the bring-up", init_mouse() == -1);
    CHECK("the drain reads PS2_DRAIN_LIMIT bytes", data_reads == PS2_DRAIN_LIMIT);
    CHECK("a stuck output buffer is not written", port_writes == 0);
    CHECK("a stuck output buffer is reported", warned("the 8042 buffer never drained"));
    CHECK("a stuck output buffer registers no handler", registrations == 0);
    CHECK("a stuck output buffer turns interrupts on again", irq_on);

    /* A controller that never takes a byte. */
    model_reset();
    mouse_present = 1;
    ctl_input_stuck = 1;
    CHECK("a stuck input buffer fails the bring-up", init_mouse() == -1);
    CHECK("a controller that never takes a byte is not written", port_writes == 0);
    CHECK("a stuck input buffer is reported", warned("the aux port would not enable"));
    CHECK("a stuck input buffer registers no handler", registrations == 0);
    CHECK("a stuck input buffer turns interrupts on again", irq_on);
    CHECK("a port that would not enable leaves the controller's setup standing", !i8042_failed());
    CHECK("and the keyboard running", !kbd_stopped);

    /* A controller that loses the configuration byte. */
    model_reset();
    mouse_present = 1;
    ctl_loses_config = 1;
    CHECK("a lost configuration byte fails the bring-up", init_mouse() == -1);
    CHECK("a lost configuration byte is not written back",
          writes_are(lost_config_ports, lost_config_values, 2));
    CHECK("a lost configuration byte is reported",
          warned("reading the configuration byte timed out"));
    CHECK("a lost configuration byte registers no handler", registrations == 0);
    CHECK("a lost configuration byte turns interrupts on again", irq_on);
    CHECK("a lost configuration byte fails the controller's setup", i8042_failed());
    CHECK("a lost configuration byte stops the keyboard with interrupts off",
          kbd_stopped && !kbd_stopped_with_irq_on);
}

/** `init_mouse` against a controller that answers late, as interrupts come
 * on again, when the keyboard must already be stopped. */
static void bringup_late_controller_rows(void)
{
    static const uint16_t lost_config_ports[] = {C, C};
    static const uint8_t lost_config_values[] = {0xA8, 0x20};
    static const uint16_t stalled_write_ports[] = {C, C, C};
    static const uint8_t stalled_write_values[] = {0xA8, 0x20, 0x60};

    /* The lost byte arrives as interrupts come on again. */
    model_reset();
    mouse_present = 1;
    ctl_loses_config = 1;
    kbd_wakes = 1;
    CHECK("a late configuration byte fails the bring-up", init_mouse() == -1);
    CHECK("a late configuration byte stops the keyboard with interrupts off",
          kbd_stopped && !kbd_stopped_with_irq_on);
    CHECK("a late configuration byte reaches no key handler",
          model_count == 1 && model_data[model_head] == ctl_config);
    CHECK("a late configuration byte draws no LED command",
          writes_are(lost_config_ports, lost_config_values, 2));

    /* A WRITE_CONFIG the controller takes late. */
    model_reset();
    mouse_present = 1;
    ctl_stalls_write_config = 1;
    kbd_wakes = 1;
    CHECK("a stalled WRITE_CONFIG fails the bring-up", init_mouse() == -1);
    CHECK("a stalled WRITE_CONFIG is reported", warned("the configuration byte would not write"));
    CHECK("a stalled WRITE_CONFIG fails the controller's setup", i8042_failed());
    CHECK("a stalled WRITE_CONFIG stops the keyboard with interrupts off",
          kbd_stopped && !kbd_stopped_with_irq_on);
    CHECK("a stalled WRITE_CONFIG is given no byte",
          writes_are(stalled_write_ports, stalled_write_values, 3));
    CHECK("no LED command becomes the configuration byte",
          ctl_config == (FIRMWARE_CONFIG & ~PS2_CONFIG_MOUSE_CLK));
    CHECK("a stalled WRITE_CONFIG turns interrupts on again", irq_on);
}

/** `init_mouse` against each fault of the mouse. */
static void bringup_mouse_rows(void)
{
    /* A mouse that does not answer its reset. */
    model_reset();
    mouse_present = 1;
    mouse_silent_to = MOUSE_CMD_RESET;
    CHECK("an unanswered reset is tolerated", init_mouse() == 0);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_plain();
    expect_mouse(0xF4, 1);
    CHECK("the reset is sent once and the bring-up goes on", writes_expected());
    CHECK("an unanswered reset leaves the mouse reporting", mouse_reporting);
    CHECK("an unanswered reset registers the handler", registrations == 1);

    /* A mouse that does not answer the default settings. */
    model_reset();
    mouse_present = 1;
    mouse_silent_to = MOUSE_CMD_SET_DEFAULTS;
    CHECK("an unanswered default is tolerated", init_mouse() == 0);
    CHECK("an unanswered default leaves the mouse reporting", mouse_reporting);

    /* A mouse that asks for the enable again. */
    model_reset();
    mouse_present = 1;
    mouse_resend_to = MOUSE_CMD_ENABLE;
    CHECK("a resend of the enable is answered", init_mouse() == 0);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_plain();
    expect_mouse(0xF4, 2);
    CHECK("the enable is sent again after a resend", writes_expected());
    CHECK("a resent enable leaves the mouse reporting", mouse_reporting);

    /* A mouse whose ACK of the enable is lost. */
    model_reset();
    mouse_present = 1;
    mouse_silent_to = MOUSE_CMD_ENABLE;
    CHECK("a lost ACK of the enable fails the bring-up", init_mouse() == -1);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_plain();
    expect_mouse(0xF4, 1);
    CHECK("the enable is not sent again after its timeout", writes_expected());
    CHECK("a lost ACK of the enable is reported", warned("the device did not enable reporting"));
    CHECK("a lost ACK of the enable registers no handler", registrations == 0);
    CHECK("a lost ACK of the enable turns interrupts on again", irq_on);
}

/** `init_mouse` against a mouse with a wheel, and an Explorer. */
static void bringup_wheel_rows(void)
{
    model_reset();
    mouse_present = 1;
    mouse_wheel = ID_IMPS;
    CHECK("the bring-up of a wheel mouse succeeds", init_mouse() == 0);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_extensions();
    expect_knock(200, 100, 80);
    expect_knock(200, 200, 80);
    expect_mouse(0xF3, 1);
    expect_mouse(100, 1);
    expect_mouse(0xE8, 1);
    expect_mouse(3, 1);
    expect_mouse(0xE6, 1);
    expect_mouse(0xF4, 1);
    CHECK("an answered knock is followed by the Explorer's", writes_expected());
    CHECK("a wheel mouse speaks the IntelliMouse's protocol",
          protocol == PROTO_IMPS && packet_size == 4);
    CHECK("it is ImPS/2 Generic Wheel Mouse, product PSMOUSE_IMPS, with a wheel",
          strcmp(registered_name, "ImPS/2 Generic Wheel Mouse") == 0 &&
              registered_product == PROTO_IMPS && registered_caps == (MOUSE_POINTER | MOUSE_WHEEL));
    CHECK("every byte of the knocks is read", model_count == 0);

    model_reset();
    mouse_present = 1;
    mouse_wheel = ID_IMEX;
    CHECK("the bring-up of an Explorer succeeds", init_mouse() == 0);
    expect_controller();
    expect_mouse(0xFF, 1);
    expect_mouse(0xF6, 1);
    expect_extensions();
    expect_knock(200, 100, 80);
    expect_knock(200, 200, 80);
    expect_mouse(0xF3, 1);
    expect_mouse(200, 1);
    expect_mouse(0xF3, 1);
    expect_mouse(80, 1);
    expect_mouse(0xF3, 1);
    expect_mouse(40, 1);
    expect_mouse(0xF3, 1);
    expect_mouse(100, 1);
    expect_mouse(0xE8, 1);
    expect_mouse(3, 1);
    expect_mouse(0xE6, 1);
    expect_mouse(0xF4, 1);
    CHECK("an Explorer is sent the rates of the horizontal wheel", writes_expected());
    CHECK("it speaks the Explorer's protocol", protocol == PROTO_IMEX && packet_size == 4);
    CHECK("it is ImExPS/2 Generic Explorer Mouse, product PSMOUSE_IMEX, with both wheels and "
          "five buttons",
          strcmp(registered_name, "ImExPS/2 Generic Explorer Mouse") == 0 &&
              registered_product == PROTO_IMEX &&
              registered_caps == (MOUSE_POINTER | MOUSE_WHEEL | MOUSE_HWHEEL | MOUSE_SIDE));
    CHECK("an Explorer's bring-up logs no warning", warnings == 0);
    CHECK("an Explorer's bring-up touches no port with interrupts on", ports_with_irq_on == 0);
    protocol = PROTO_PS2;
    packet_size = 3;
}

#undef C
#undef D

int main(void)
{
    handler_rows();
    wheel_rows();
    command_rows();
    bringup_controller_rows();
    bringup_late_controller_rows();
    bringup_mouse_rows();
    bringup_wheel_rows();
    if (fails == 0) {
        printf("ps2mouse-test: pass\n");
    }
    return fails ? 1 : 0;
}
