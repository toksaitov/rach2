/**
 * The PS/2 mouse's bring-up, its IRQ 12 handler and its commands, driven on
 * the host against a model of the 8042 and the mouse behind it
 * (sys/arch/x86/ps2mouse.c).
 *
 * The file is included whole so that the static functions and the packet
 * state are reachable, with the 8042's waits of i8042.c. The model's output buffer is a queue of
 * bytes, each with its status: the status register reads as the first one's, and a read of the data
 * port is counted and takes it. A status with the output buffer full (bit 0) and bit 5 clear marks
 * a keyboard byte, which the handler must leave unread for the keyboard's IRQ 1 handler, with the
 * packet as it was. With bit 5 set the byte is the mouse's: the handler reads it, and three make a
 * packet. A command to the mouse takes as its reply only a byte marked as the mouse's, and drops a
 * keyboard byte that comes first.
 *
 * Every port write is kept in order. A write to port 0x64 is a command to the
 * controller: 0xA8 and 0xA7 clear and set the configuration byte's
 * aux-clock-disable bit, 0x20 queues the configuration byte, and 0x60 and
 * 0xD4 send the next write to port 0x60 to the configuration byte and to the
 * mouse. The mouse answers only while its clock is enabled: an ACK to each
 * command, 0xAA and its ID 0x00 after the ACK of a reset, and reporting on
 * after an ACK of 0xF4. A row can make the mouse lose its answer to one
 * command, or ask once for that command again with 0xFE, and can make the
 * controller lose the configuration byte, keep its input buffer full or keep
 * its output buffer full for good. The rows of the bring-up check the
 * writes in order, the state the controller and the mouse are left in, the
 * vector of the handler, and that interrupts are off for every port access
 * and on again at every return.
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
#define MODEL_WRITES 32
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
static int ctl_output_stuck;

/* The mouse: whether one is there, a command it never answers and one it
 * first answers with MOUSE_RESEND (-1 for none), and whether it reports. */
static int mouse_present;
static int mouse_silent_to;
static int mouse_resend_to;
static int mouse_reporting;

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

/* The packets the handler handed to the mouse queue. */
static int pushes;
static uint8_t pushed_buttons;
static int pushed_dx;
static int pushed_dy;

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
    ctl_output_stuck = 0;
    mouse_present = 0;
    mouse_silent_to = -1;
    mouse_resend_to = -1;
    mouse_reporting = 0;
    irq_on = 1;
    ports_with_irq_on = 0;
    registrations = 0;
    registered_vector = 0;
    registered_handler = 0;
    registered_with_irq_on = 0;
    warnings = 0;
    last_warning = 0;
}

/** Take `cmd`, a byte the controller sent on to the mouse. */
static void mouse_take(uint8_t cmd)
{
    if (!mouse_present || (ctl_config & PS2_CONFIG_MOUSE_CLK) != 0 || cmd == mouse_silent_to) {
        return;
    }
    if (cmd == mouse_resend_to) {
        mouse_resend_to = -1;
        model_put(MOUSE_BYTE, MOUSE_RESEND);
        return;
    }
    model_put(MOUSE_BYTE, MOUSE_ACK);
    if (cmd == MOUSE_CMD_RESET) {
        mouse_reporting = 0;
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
        } else if (model_count > 0) {
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

void arch_irq_enable(void)
{
    irq_on = 1;
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

void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    pushes++;
    pushed_buttons = buttons;
    pushed_dx = dx;
    pushed_dy = dy;
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
    CHECK("byte 1 is read", interrupt(MOUSE_BYTE, 0xFE) == 1);
    CHECK("byte 2 is read", interrupt(MOUSE_BYTE, 0xFD) == 1);
    CHECK("three mouse bytes make one packet", pushes == 1);
    CHECK("the packet starts the next at byte 0", packet_index == 0);
    CHECK("the packet has the left button", pushed_buttons == 0x01);
    CHECK("the packet's dx is -2", pushed_dx == -2);
    CHECK("the packet's dy is -3, as the wire gives it", pushed_dy == -3);

    /* A byte out of step with the stream, in the place of byte 0. */
    CHECK("a byte without bit 3 is read", interrupt(MOUSE_BYTE, 0x02) == 1);
    CHECK("a first byte without bit 3 is dropped", packet_index == 0);

    /* A packet whose X movement overflowed. */
    interrupt(MOUSE_BYTE, 0x48);
    interrupt(MOUSE_BYTE, 0x10);
    interrupt(MOUSE_BYTE, 0x10);
    CHECK("a packet with an overflow bit is dropped", pushes == 1);
    CHECK("a dropped packet starts the next at byte 0", packet_index == 0);

    CHECK("the handler writes no port", port_writes == 0);
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

    /* A key and nothing from the mouse: three tries, each timed out. */
    model_reset();
    model_put(KEYBOARD_BYTE, 0x1C);
    CHECK("a key alone is no reply", mouse_send_command(MOUSE_CMD_ENABLE) == -1);
    CHECK("the command is tried three times", port_writes == 6);

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

/** `init_mouse` against a working controller and mouse, and against each
 * fault of the controller. */
static void bringup_controller_rows(void)
{
    static const uint16_t whole_ports[] = {C, C, C, D, C, D, C, D, C, D};
    static const uint8_t whole_values[] = {0xA8, 0x20, 0x60, MOUSE_CONFIG, 0xD4,
                                           0xFF, 0xD4, 0xF6, 0xD4,         0xF4};
    static const uint16_t lost_config_ports[] = {C, C};
    static const uint8_t lost_config_values[] = {0xA8, 0x20};

    /* A working controller, with a key and a mouse byte the firmware left. */
    model_reset();
    mouse_present = 1;
    model_put(KEYBOARD_BYTE, 0x1C);
    model_put(MOUSE_BYTE, 0x08);
    CHECK("the bring-up on a working controller succeeds", init_mouse() == 0);
    CHECK("the controller is written the bring-up's sequence",
          writes_are(whole_ports, whole_values, 10));
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
}

/** `init_mouse` against each fault of the mouse. */
static void bringup_mouse_rows(void)
{
    static const uint16_t no_reset_ports[] = {C, C, C, D, C, D, C, D, C, D, C, D, C, D};
    static const uint8_t no_reset_values[] = {0xA8, 0x20, 0x60, MOUSE_CONFIG, 0xD4, 0xFF, 0xD4,
                                              0xFF, 0xD4, 0xFF, 0xD4,         0xF6, 0xD4, 0xF4};
    static const uint16_t resend_ports[] = {C, C, C, D, C, D, C, D, C, D, C, D};
    static const uint8_t resend_values[] = {0xA8, 0x20, 0x60, MOUSE_CONFIG, 0xD4, 0xFF,
                                            0xD4, 0xF6, 0xD4, 0xF4,         0xD4, 0xF4};
    static const uint16_t no_enable_ports[] = {C, C, C, D, C, D, C, D, C, D, C, D, C, D};
    static const uint8_t no_enable_values[] = {0xA8, 0x20, 0x60, MOUSE_CONFIG, 0xD4, 0xFF, 0xD4,
                                               0xF6, 0xD4, 0xF4, 0xD4,         0xF4, 0xD4, 0xF4};

    /* A mouse that does not answer its reset. */
    model_reset();
    mouse_present = 1;
    mouse_silent_to = MOUSE_CMD_RESET;
    CHECK("an unanswered reset is tolerated", init_mouse() == 0);
    CHECK("the reset is tried three times and the bring-up goes on",
          writes_are(no_reset_ports, no_reset_values, 14));
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
    CHECK("the enable is sent again after a resend", writes_are(resend_ports, resend_values, 12));
    CHECK("a resent enable leaves the mouse reporting", mouse_reporting);

    /* A mouse whose ACK of the enable is lost. */
    model_reset();
    mouse_present = 1;
    mouse_silent_to = MOUSE_CMD_ENABLE;
    CHECK("a lost ACK of the enable fails the bring-up", init_mouse() == -1);
    CHECK("the enable is tried three times", writes_are(no_enable_ports, no_enable_values, 14));
    CHECK("a lost ACK of the enable is reported", warned("the device did not enable reporting"));
    CHECK("a lost ACK of the enable registers no handler", registrations == 0);
    CHECK("a lost ACK of the enable turns interrupts on again", irq_on);
}

#undef C
#undef D

int main(void)
{
    handler_rows();
    command_rows();
    bringup_controller_rows();
    bringup_mouse_rows();
    if (fails == 0) {
        printf("ps2mouse-test: pass\n");
    }
    return fails ? 1 : 0;
}
