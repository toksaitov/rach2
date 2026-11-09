/*
 * ps2mouse.c: the PS/2 mouse, on the 8042's auxiliary port, as ps2mouse.h
 * describes it. The mouse shares the controller with the keyboard: both use
 * port 0x60 for data and 0x64 for status and commands, and the waits around a
 * write are i8042.c's. The mouse interrupts on IRQ 12, vector 44.
 *
 * A packet is three bytes:
 *   Byte 0: [Yovf|Xovf|Ysign|Xsign|1|Middle|Right|Left]
 *   Byte 1: X movement, the low 8 bits of a 9-bit value, Xsign its sign
 *   Byte 2: Y movement, the low 8 bits of a 9-bit value, Ysign its sign
 * A mouse that answers Microsoft's knocks, sample rates set in a fixed order
 * and then its ID read, sends a fourth: the IntelliMouse (ID 3) its wheel's
 * turn as a signed byte, and the IntelliMouse Explorer (ID 4) a wheel's turn
 * with two more buttons, decoded as Linux's psmouse decodes them. The event
 * device has Linux's name, numbers and capabilities for each protocol.
 */
#include "i8042.h"
#include "portio.h"
#include "ps2mouse.h"
#include "../arch.h"
#include "../../platform.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../mouse.h"
#include "isr.h"
#include "../../kio.h"
#include "../../serial.h"

/* 8042 Controller Commands (write to 0x64) */
#define PS2_CMD_READ_CONFIG   0x20 /* Read command byte */
#define PS2_CMD_WRITE_CONFIG  0x60 /* Write command byte */
#define PS2_CMD_DISABLE_MOUSE 0xA7 /* Disable auxiliary port */
#define PS2_CMD_ENABLE_MOUSE  0xA8 /* Enable auxiliary port */
#define PS2_CMD_TEST_MOUSE    0xA9 /* Test auxiliary port */
#define PS2_CMD_WRITE_MOUSE   0xD4 /* Write to auxiliary device */

/* Mouse Commands (sent via PS2_CMD_WRITE_MOUSE) */
#define MOUSE_CMD_RESET        0xFF /* Reset mouse */
#define MOUSE_CMD_SET_RES      0xE8 /* Set resolution */
#define MOUSE_CMD_SET_SCALE11  0xE6 /* Set 1:1 scaling */
#define MOUSE_CMD_RESEND       0xFE /* Resend last packet */
#define MOUSE_CMD_SET_DEFAULTS 0xF6 /* Set default settings */
#define MOUSE_CMD_DISABLE      0xF5 /* Disable data reporting */
#define MOUSE_CMD_ENABLE       0xF4 /* Enable data reporting */
#define MOUSE_CMD_SET_RATE     0xF3 /* Set sample rate */
#define MOUSE_CMD_GET_ID       0xF2 /* Get device ID */

/* Mouse Response Codes */
#define MOUSE_ACK    0xFA /* Command acknowledged */
#define MOUSE_RESEND 0xFE /* Resend request */

/* Configuration byte bits */
#define PS2_CONFIG_MOUSE_IRQ 0x02 /* Enable IRQ12 for mouse */
#define PS2_CONFIG_MOUSE_CLK 0x20 /* Disable mouse clock (set=disabled) */

/* The protocols, Linux's `enum psmouse_type`, which EVIOCGID gives as the
 * product, and the IDs a mouse answers once it speaks the last two. */
#define PROTO_PS2  1
#define PROTO_IMPS 5
#define PROTO_IMEX 6
#define ID_IMPS    3
#define ID_IMEX    4

/* The packet being assembled, the index of its next byte, and its size,
 * three bytes or four with a wheel. */
static uint8_t packet_bytes[4];
static int packet_index = 0;
static int packet_size = 3;

/* The protocol the mouse speaks, PROTO_*, and its event device. */
static int protocol = PROTO_PS2;
static struct evdev *mouse_dev;

/* The side and extra buttons held, MOUSE_BUTTON_*: an Explorer's packet
 * of a wheel's turn of more than 7 does not carry them. */
static unsigned int extra_buttons;

/* How many bytes the first drain reads before it calls the controller
   stuck. A PS/2 output buffer has one byte, so anything past a handful is a
   controller that is not answering. */
#define PS2_DRAIN_LIMIT 4096

/**
 * Spin until the 8042 output buffer has a byte from the mouse, ready to read.
 *
 * A keyboard byte waiting in the buffer is read and dropped: the buffer holds
 * one byte, so the mouse's cannot arrive until the keyboard's is taken. A key
 * pressed while the mouse is brought up is lost.
 *
 * @return 0 when a mouse byte is present, -1 on timeout.
 */
static int ps2_wait_mouse_output(void)
{
    int timeout = PS2_TIMEOUT;
    uint8_t status;

    while (timeout-- > 0) {
        status = inb(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0) {
            continue;
        }
        if (status & PS2_STATUS_MOUSE_DATA) {
            return 0;
        }
        inb(PS2_DATA_PORT);
    }
    return -1;
}

/**
 * Send a byte to the mouse through the 8042 auxiliary port.
 *
 * Each command must be prefixed with PS2_CMD_WRITE_MOUSE so the
 * controller routes the following data byte to the auxiliary device. The
 * reply is the next byte the status marks as the mouse's, and a keyboard
 * byte before it is dropped. A MOUSE_RESEND reply means the mouse saw a
 * framing error, and a timeout means the controller or the mouse did not
 * answer; either way the command is tried again, three times in all. Any
 * other byte is returned to the caller, since some commands answer with
 * data and not an ACK.
 *
 * @return the mouse's response byte, or -1 after three failed tries.
 */
static int mouse_send_command(uint8_t cmd)
{
    int retries = 3;
    uint8_t response;

    while (retries-- > 0) {
        /* Prefix tells the 8042 to forward the next byte to the mouse. */
        if (ps2_send_command(PS2_CMD_WRITE_MOUSE) < 0) {
            continue;
        }

        if (ps2_wait_input() < 0) {
            continue;
        }
        outb(PS2_DATA_PORT, cmd);

        if (ps2_wait_mouse_output() < 0) {
            continue;
        }

        response = inb(PS2_DATA_PORT);
        if (response == MOUSE_ACK) {
            return response;
        } else if (response == MOUSE_RESEND) {
            continue;
        } else {
            return response;
        }
    }

    return -1;
}

/**
 * Reset the mouse and read the two bytes it sends after its ACK, 0xAA for a
 * passed self-test and 0x00 for its device ID. Their values are not checked,
 * and a timeout on either is tolerated.
 *
 * @return 0 when the mouse answered the reset, -1 when it did not.
 */
static int mouse_reset(void)
{
    if (mouse_send_command(MOUSE_CMD_RESET) < 0) {
        return -1;
    }
    if (ps2_wait_mouse_output() == 0) {
        inb(PS2_DATA_PORT);
    }
    if (ps2_wait_mouse_output() == 0) {
        inb(PS2_DATA_PORT);
    }
    return 0;
}

/**
 * Send the three sample rates of `rates` and read the ID the mouse answers
 * then: one of Microsoft's knocks, which a mouse with a wheel answers by
 * speaking its protocol from then on. Returns the ID, or -1 when the mouse
 * did not answer.
 */
static int knock(const uint8_t *rates)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (mouse_send_command(MOUSE_CMD_SET_RATE) != MOUSE_ACK ||
            mouse_send_command(rates[i]) != MOUSE_ACK) {
            return -1;
        }
    }
    if (mouse_send_command(MOUSE_CMD_GET_ID) != MOUSE_ACK || ps2_wait_mouse_output() < 0) {
        return -1;
    }
    return inb(PS2_DATA_PORT);
}

/** Send `cmd` and its argument `arg`; a refusal is ignored, since the
 * mouse works on at its old setting. */
static void mouse_set(uint8_t cmd, uint8_t arg)
{
    if (mouse_send_command(cmd) == MOUSE_ACK) {
        mouse_send_command(arg);
    }
}

/**
 * Find the protocol the mouse speaks, as Linux's psmouse does, and set it as
 * Linux sets it: the IntelliMouse's knock, rates 200, 100 and 80, then the
 * Explorer's, 200, 200 and 80, with the rates 200, 80 and 40 that turn on an
 * Explorer 4.0's horizontal wheel; then 100 samples a second, 8 counts a
 * millimetre and 1:1 scaling. A mouse that answers neither knock is a plain
 * PS/2 mouse of three buttons.
 */
static void detect_protocol(void)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    int i;

    protocol = PROTO_PS2;
    packet_size = 3;
    if (knock(imps) == ID_IMPS) {
        protocol = PROTO_IMPS;
        packet_size = 4;
        if (knock(imex) == ID_IMEX) {
            protocol = PROTO_IMEX;
            for (i = 0; i < 3; i++) {
                mouse_set(MOUSE_CMD_SET_RATE, hwheel[i]);
            }
        }
    }
    mouse_set(MOUSE_CMD_SET_RATE, 100);
    mouse_set(MOUSE_CMD_SET_RES, 3);
    mouse_send_command(MOUSE_CMD_SET_SCALE11);
}

/** Register the event device of the mouse, with Linux's name, numbers and
 * capabilities for its protocol. */
static void register_mouse(void)
{
    const char *name = "PS/2 Generic Mouse";
    unsigned int caps = MOUSE_POINTER;

    if (protocol == PROTO_IMPS) {
        name = "ImPS/2 Generic Wheel Mouse";
        caps |= MOUSE_WHEEL;
    } else if (protocol == PROTO_IMEX) {
        name = "ImExPS/2 Generic Explorer Mouse";
        caps |= MOUSE_WHEEL | MOUSE_HWHEEL | MOUSE_SIDE;
    }
    mouse_dev = mouse_register(name, "isa0060/serio1/input0", EVDEV_BUS_I8042, 0x0002,
                               (uint16_t)protocol, caps);
}

/** The low `bits` + 1 bits of `v` as a signed number, bit `bits` its sign,
 * as Linux's `sign_extend32`. */
static int sign_extend(unsigned v, int bits)
{
    int sign = 1 << bits;

    return (int)((v & ((unsigned)sign * 2u - 1u)) ^ (unsigned)sign) - sign;
}

/**
 * Report the whole packet in `packet_bytes`: its buttons, its 9-bit
 * movements, Y turned to Linux's way up, and the fourth byte of a wheel's
 * protocol, as Linux's `psmouse_process_byte` reads it. A packet with an
 * overflow bit set, bit 6 or 7, has a movement that cannot be used, and is
 * dropped.
 */
static void report_packet(void)
{
    uint8_t flags = packet_bytes[0];
    uint8_t b3 = packet_bytes[3];
    int dx = packet_bytes[1];
    int dy = packet_bytes[2];
    int wheel = 0, hwheel = 0;

    if (flags & 0xC0) {
        return;
    }
    if (flags & 0x10) {
        dx -= 256;
    }
    if (flags & 0x20) {
        dy -= 256;
    }
    if (protocol == PROTO_IMPS) {
        wheel = -(int)(int8_t)b3;
    } else if (protocol == PROTO_IMEX) {
        switch (b3 & 0xC0) {
        case 0x80: /* an Explorer 4.0's vertical turn of up to 31 */
            wheel = -sign_extend(b3, 5);
            break;
        case 0x40: /* its horizontal turn */
            hwheel = -sign_extend(b3, 5);
            break;
        default:
            wheel = -sign_extend(b3, 3);
            extra_buttons =
                ((b3 & 0x10) ? MOUSE_BUTTON_SIDE : 0) | ((b3 & 0x20) ? MOUSE_BUTTON_EXTRA : 0);
            break;
        }
    }
    mouse_report(mouse_dev, (flags & 0x07u) | extra_buttons, dx, -dy, wheel, hwheel);
}

/**
 * The IRQ 12 handler, which assembles a packet one byte an interrupt and
 * reports it.
 *
 * The 8042 raises IRQ 12 for every auxiliary byte. The handler reads the
 * data port only when status bit 0 (PS2_STATUS_OUTPUT_FULL) says a byte
 * waits and bit 5 (PS2_STATUS_MOUSE_DATA) says it is the auxiliary
 * device's. A keyboard byte is left in the output buffer for the keyboard's
 * IRQ 1 handler, and the packet is left as it was.
 *
 * A packet is framed by byte 0, whose bit 3 is always 1: a byte expected as
 * byte 0 with that bit clear is out of step with the stream and is dropped,
 * which brings the stream back in step.
 */
static void mouse_irq_handler(isr_params_t *params)
{
    uint8_t status;
    uint8_t data;

    (void)params;
    status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || !(status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }
    data = inb(PS2_DATA_PORT);

    /* Byte 0 has the always-1 framing bit (bit 3). */
    if (packet_index == 0) {
        if (!(data & 0x08)) {
            return;
        }
    }

    packet_bytes[packet_index++] = data;

    if (packet_index >= packet_size) {
        report_packet();
        packet_index = 0;
    }
}

/**
 * Interrupts are off so that the handshake's bytes are not taken by a stray
 * IRQ 12. The output buffer is drained first of what the firmware left in
 * it, at most PS2_DRAIN_LIMIT bytes: the buffer is one byte deep and a
 * working controller clears in a read or two, but one that reports a full
 * buffer for good would otherwise stop the boot here, with interrupts off
 * and nothing said, which on a laptop without a serial port leaves nothing
 * to read. A failed reset is tolerated because some emulators do not answer
 * it, and so is a refused default; a failed port enable, configuration byte
 * or reporting enable fails the bring-up.
 */
int init_mouse(void)
{
    uint8_t config;
    int response;

    arch_irq_disable();

    {
        int drained = PS2_DRAIN_LIMIT;

        while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            inb(PS2_DATA_PORT);
            if (--drained <= 0) {
                klog(KLOG_WARN, "mouse", "the 8042 buffer never drained");
                arch_irq_enable();
                return -1;
            }
        }
    }

    if (ps2_send_command(PS2_CMD_ENABLE_MOUSE) < 0) {
        klog(KLOG_WARN, "mouse", "the aux port would not enable");
        arch_irq_enable();
        return -1;
    }

    if (ps2_send_command(PS2_CMD_READ_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not read");
        arch_irq_enable();
        return -1;
    }
    if (ps2_wait_output() < 0) {
        klog(KLOG_WARN, "mouse", "reading the configuration byte timed out");
        arch_irq_enable();
        return -1;
    }
    config = inb(PS2_DATA_PORT);

    config |= PS2_CONFIG_MOUSE_IRQ;  /* enable IRQ12 */
    config &= ~PS2_CONFIG_MOUSE_CLK; /* clear the clock-disable bit */

    if (ps2_send_command(PS2_CMD_WRITE_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not write");
        arch_irq_enable();
        return -1;
    }
    if (ps2_wait_input() < 0) {
        klog(KLOG_WARN, "mouse", "writing the configuration byte timed out");
        arch_irq_enable();
        return -1;
    }
    outb(PS2_DATA_PORT, config);

    if (mouse_reset() < 0) {
        klog(KLOG_WARN, "mouse", "the device did not answer its reset");
    }

    response = mouse_send_command(MOUSE_CMD_SET_DEFAULTS);
    if (response < 0) {
        klog(KLOG_WARN, "mouse", "the device refused the default settings");
    }

    detect_protocol();
    register_mouse();

    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        arch_irq_enable();
        return -1;
    }

    register_isr_handler(44, mouse_irq_handler, "mouse");

    arch_irq_enable();

    return 0;
}

/** `mouse_clear_packets` calls this when the queue is emptied, so that half
 * a packet assembled before is not finished with the bytes after. */
void platform_mouse_resync(void)
{
    packet_index = 0;
}
