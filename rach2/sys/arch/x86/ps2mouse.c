/*
 * ps2mouse.c: the PS/2 mouse, on the 8042's auxiliary port, as ps2mouse.h
 * describes it. The mouse shares the controller with the keyboard: both use
 * port 0x60 for data and 0x64 for status and commands. The mouse interrupts
 * on IRQ 12, vector 44.
 *
 * A packet is three bytes:
 *   Byte 0: [Yovf|Xovf|Ysign|Xsign|1|Middle|Right|Left]
 *   Byte 1: X movement, the low 8 bits of a 9-bit value, Xsign its sign
 *   Byte 2: Y movement, the low 8 bits of a 9-bit value, Ysign its sign
 */
#include "portio.h"
#include "ps2mouse.h"
#include "../../platform.h"
#include "../../klog.h"
#include "../../mouse.h"
#include "isr.h"
#include "../../kio.h"
#include "../../serial.h"

/* 8042 Controller Ports */
#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

/* Status Register Bits */
#define PS2_STATUS_OUTPUT_FULL 0x01 /* Output buffer full (data ready) */
#define PS2_STATUS_INPUT_FULL  0x02 /* Input buffer full (can't write) */
#define PS2_STATUS_MOUSE_DATA  0x20 /* Data is from mouse, not keyboard */

/* 8042 Controller Commands (write to 0x64) */
#define PS2_CMD_READ_CONFIG   0x20 /* Read command byte */
#define PS2_CMD_WRITE_CONFIG  0x60 /* Write command byte */
#define PS2_CMD_DISABLE_MOUSE 0xA7 /* Disable auxiliary port */
#define PS2_CMD_ENABLE_MOUSE  0xA8 /* Enable auxiliary port */
#define PS2_CMD_TEST_MOUSE    0xA9 /* Test auxiliary port */
#define PS2_CMD_WRITE_MOUSE   0xD4 /* Write to auxiliary device */

/* Mouse Commands (sent via PS2_CMD_WRITE_MOUSE) */
#define MOUSE_CMD_RESET        0xFF /* Reset mouse */
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

/* Timeout for waiting on 8042 controller (iterations) */
#define PS2_TIMEOUT 100000

/* The packet being assembled, and the index of its next byte. */
static uint8_t packet_bytes[3];
static int packet_index = 0;

/* How many bytes the first drain reads before it calls the controller
   stuck. A PS/2 output buffer has one byte, so anything past a handful is a
   controller that is not answering. */
#define PS2_DRAIN_LIMIT 4096

/**
 * Spin until the 8042 input buffer is empty, ready to accept a byte.
 *
 * @return 0 when the buffer drains, -1 on timeout.
 */
static int ps2_wait_input(void)
{
    int timeout = PS2_TIMEOUT;
    while (timeout-- > 0) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

/**
 * Spin until the 8042 output buffer has a byte, ready to read.
 *
 * @return 0 when data is present, -1 on timeout.
 */
static int ps2_wait_output(void)
{
    int timeout = PS2_TIMEOUT;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return 0;
        }
    }
    return -1;
}

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
 * Write a command byte to the 8042 controller's command port.
 *
 * @return 0 on success, -1 if the input buffer never drains.
 */
static int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_input() < 0)
        return -1;
    outb(PS2_CMD_PORT, cmd);
    return 0;
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
 * The IRQ 12 handler, which assembles a three-byte packet one byte an
 * interrupt and puts it in the mouse queue.
 *
 * The 8042 raises IRQ 12 for every auxiliary byte. The handler reads the
 * data port only when status bit 0 (PS2_STATUS_OUTPUT_FULL) says a byte
 * waits and bit 5 (PS2_STATUS_MOUSE_DATA) says it is the auxiliary
 * device's. A keyboard byte is left in the output buffer for the keyboard's
 * IRQ 1 handler, and the packet is left as it was.
 *
 * A packet is framed by byte 0, whose bit 3 is always 1: a byte expected as
 * byte 0 with that bit clear is out of step with the stream and is dropped,
 * which brings the stream back in step. The X and Y movements are 9-bit
 * signed values with their high bits in bits 4 and 5 of byte 0; a set
 * overflow bit, bit 6 or 7, marks a movement that cannot be used, and the
 * whole packet is discarded.
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

    if (packet_index >= 3) {
        uint8_t flags = packet_bytes[0];
        int dx = packet_bytes[1];
        int dy = packet_bytes[2];

        if (flags & 0x10) { /* X sign bit: extend the 9th bit */
            dx |= 0xFFFFFF00;
        }
        if (flags & 0x20) { /* Y sign bit: extend the 9th bit */
            dy |= 0xFFFFFF00;
        }

        if (!(flags & 0xC0)) { /* no X or Y overflow */
            uint8_t buttons = flags & 0x07;

            mouse_push_packet(buttons, dx, dy);
        }

        packet_index = 0;
    }
}

/* A host test compiles the handler and the mouse's commands without the
 * bring-up, whose `cli` and `sti` it can neither assemble on every host nor
 * run. */
#ifndef RACH2_HOSTTEST

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

    __asm__ __volatile__("cli");

    {
        int drained = PS2_DRAIN_LIMIT;

        while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            inb(PS2_DATA_PORT);
            if (--drained <= 0) {
                klog(KLOG_WARN, "mouse", "the 8042 buffer never drained");
                __asm__ __volatile__("sti");
                return -1;
            }
        }
    }

    if (ps2_send_command(PS2_CMD_ENABLE_MOUSE) < 0) {
        klog(KLOG_WARN, "mouse", "the aux port would not enable");
        __asm__ __volatile__("sti");
        return -1;
    }

    if (ps2_send_command(PS2_CMD_READ_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not read");
        __asm__ __volatile__("sti");
        return -1;
    }
    if (ps2_wait_output() < 0) {
        klog(KLOG_WARN, "mouse", "reading the configuration byte timed out");
        __asm__ __volatile__("sti");
        return -1;
    }
    config = inb(PS2_DATA_PORT);

    config |= PS2_CONFIG_MOUSE_IRQ;  /* enable IRQ12 */
    config &= ~PS2_CONFIG_MOUSE_CLK; /* clear the clock-disable bit */

    if (ps2_send_command(PS2_CMD_WRITE_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not write");
        __asm__ __volatile__("sti");
        return -1;
    }
    if (ps2_wait_input() < 0) {
        klog(KLOG_WARN, "mouse", "writing the configuration byte timed out");
        __asm__ __volatile__("sti");
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

    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        __asm__ __volatile__("sti");
        return -1;
    }

    register_isr_handler(44, mouse_irq_handler, "mouse");

    __asm__ __volatile__("sti");

    return 0;
}

#endif /* !RACH2_HOSTTEST */

/** `mouse_clear_packets` calls this when the queue is emptied, so that half
 * a packet assembled before is not finished with the bytes after. */
void platform_mouse_resync(void)
{
    packet_index = 0;
}
