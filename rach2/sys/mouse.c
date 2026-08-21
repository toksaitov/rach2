#include "mouse.h"
#include "isr.h"
#include "kio.h"
#include "serial.h"

/*
 * PS/2 Mouse Driver
 *
 * The PS/2 mouse shares the 8042 controller with the keyboard.
 * Both use port 0x60 for data and 0x64 for status/commands.
 * The mouse uses IRQ12 (mapped to vector 44 after PIC remapping).
 *
 * Mouse packets are 3 bytes:
 *   Byte 0: [Yovf|Xovf|Ysign|Xsign|1|Middle|Right|Left]
 *   Byte 1: X movement (8-bit, use Xsign for 9-bit signed)
 *   Byte 2: Y movement (8-bit, use Ysign for 9-bit signed)
 */

/* 8042 Controller Ports */
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

/* Status Register Bits */
#define PS2_STATUS_OUTPUT_FULL  0x01  /* Output buffer full (data ready) */
#define PS2_STATUS_INPUT_FULL   0x02  /* Input buffer full (can't write) */
#define PS2_STATUS_MOUSE_DATA   0x20  /* Data is from mouse, not keyboard */

/* 8042 Controller Commands (write to 0x64) */
#define PS2_CMD_READ_CONFIG     0x20  /* Read command byte */
#define PS2_CMD_WRITE_CONFIG    0x60  /* Write command byte */
#define PS2_CMD_DISABLE_MOUSE   0xA7  /* Disable auxiliary port */
#define PS2_CMD_ENABLE_MOUSE    0xA8  /* Enable auxiliary port */
#define PS2_CMD_TEST_MOUSE      0xA9  /* Test auxiliary port */
#define PS2_CMD_WRITE_MOUSE     0xD4  /* Write to auxiliary device */

/* Mouse Commands (sent via PS2_CMD_WRITE_MOUSE) */
#define MOUSE_CMD_RESET         0xFF  /* Reset mouse */
#define MOUSE_CMD_RESEND        0xFE  /* Resend last packet */
#define MOUSE_CMD_SET_DEFAULTS  0xF6  /* Set default settings */
#define MOUSE_CMD_DISABLE       0xF5  /* Disable data reporting */
#define MOUSE_CMD_ENABLE        0xF4  /* Enable data reporting */
#define MOUSE_CMD_SET_RATE      0xF3  /* Set sample rate */
#define MOUSE_CMD_GET_ID        0xF2  /* Get device ID */

/* Mouse Response Codes */
#define MOUSE_ACK               0xFA  /* Command acknowledged */
#define MOUSE_RESEND            0xFE  /* Resend request */

/* Configuration byte bits */
#define PS2_CONFIG_MOUSE_IRQ    0x02  /* Enable IRQ12 for mouse */
#define PS2_CONFIG_MOUSE_CLK    0x20  /* Disable mouse clock (set=disabled) */

/* Timeout for waiting on 8042 controller (iterations) */
#define PS2_TIMEOUT             100000

/* Mouse packet ring buffer */
#define MOUSE_BUF_SIZE 64
static struct mouse_packet mouse_buf[MOUSE_BUF_SIZE];
static volatile int mouse_head = 0;
static volatile int mouse_tail = 0;

/* Packet assembly state */
static uint8_t packet_bytes[3];
static int packet_index = 0;

/* Driver state */
static bool mouse_initialized = false;

/**
 * Spin until the 8042 input buffer is empty, ready to accept a byte.
 *
 * @return 0 when the buffer drains, -1 on timeout.
 */
/* How many bytes the initial drain will read before calling the controller
   stuck. A PS/2 output buffer has one byte, so anything past a handful is a
   controller that is not answering. */
#define PS2_DRAIN_LIMIT 4096

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
 * Write a command byte to the 8042 controller's command port.
 *
 * @return 0 on success, -1 if the input buffer never drains.
 */
static int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_input() < 0) return -1;
    outb(PS2_CMD_PORT, cmd);
    return 0;
}

/**
 * Send a byte to the mouse through the 8042 auxiliary port.
 *
 * Each command must be prefixed with PS2_CMD_WRITE_MOUSE so the
 * controller routes the following data byte to the auxiliary device.
 * A MOUSE_RESEND reply means the mouse saw a framing error; retry up
 * to three times. Any other non-ACK byte is returned to the caller,
 * since some commands answer with data, not an ACK.
 *
 * @return the mouse's response byte, or -1 after the retries are spent.
 */
static int mouse_send_command(uint8_t cmd)
{
    int retries = 3;

    while (retries-- > 0) {
        /* Prefix tells the 8042 to forward the next byte to the mouse. */
        if (ps2_send_command(PS2_CMD_WRITE_MOUSE) < 0) {
            continue;
        }

        if (ps2_wait_input() < 0) {
            continue;
        }
        outb(PS2_DATA_PORT, cmd);

        if (ps2_wait_output() < 0) {
            continue;
        }

        uint8_t response = inb(PS2_DATA_PORT);

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
 * Append a completed packet to the ring buffer, dropping it when full.
 *
 * Runs in IRQ context (the producer); the consumer is mouse_read_packets.
 * Deltas are clamped to the int8_t range the packet struct has.
 */
static void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    int next = (mouse_tail + 1) % MOUSE_BUF_SIZE;
    if (next != mouse_head) {
        if (dx > 127) dx = 127;
        if (dx < -128) dx = -128;
        if (dy > 127) dy = 127;
        if (dy < -128) dy = -128;

        mouse_buf[mouse_tail].buttons = buttons & 0x07;  /* button bits 0-2 */
        mouse_buf[mouse_tail].dx = (int8_t)dx;
        mouse_buf[mouse_tail].dy = (int8_t)dy;
        mouse_tail = next;
    }
}

/**
 * IRQ12 handler that assembles 3-byte PS/2 packets one byte per interrupt.
 *
 * The 8042 raises IRQ12 for every auxiliary byte; status bit 5
 * (PS2_STATUS_MOUSE_DATA) distinguishes mouse bytes from keyboard bytes
 * that share the controller. Packets are framed by byte 0, whose bit 3
 * is always 1: if that bit is clear while we expect byte 0, the stream
 * is out of sync and the byte is dropped to resynchronize. The X and Y
 * movement bytes are 9-bit signed, with the high bit in flags bits 4-5;
 * a set overflow bit (flags bits 6-7) marks an unusable delta and the
 * whole packet is discarded.
 */
static void mouse_irq_handler(isr_params_t *params)
{
    (void)params;

    uint8_t status = inb(PS2_STATUS_PORT);

    if (!(status & PS2_STATUS_OUTPUT_FULL)) {
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    /* Keyboard byte sharing the 8042; its own handler will read it. */
    if (!(status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }

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

        if (flags & 0x10) {  /* X sign bit: extend the 9th bit */
            dx |= 0xFFFFFF00;
        }
        if (flags & 0x20) {  /* Y sign bit: extend the 9th bit */
            dy |= 0xFFFFFF00;
        }

        if (!(flags & 0xC0)) {  /* no X or Y overflow */
            uint8_t buttons = flags & 0x07;

            /* PS/2 Y grows upward; negate so positive dy is forward. */
            mouse_push_packet(buttons, dx, -dy);
        }

        packet_index = 0;
    }
}

/**
 * Bring up the PS/2 mouse: enable the aux port, IRQ12, and data reporting.
 *
 * Runs with interrupts masked so the configuration handshake is not
 * interleaved with stray IRQ12 bytes. The output buffer is drained first
 * to discard junk left by firmware. A failed reset is tolerated because
 * some emulators do not answer it; a failed port enable or data-reporting
 * enable is fatal. The mouse uses IRQ12, vector 44 (32 + 12) after PIC
 * remapping.
 *
 * @return 0 once data reporting is on, -1 on a fatal handshake failure.
 */
int init_mouse(void)
{
    uint8_t config;
    int response;

    __asm__ __volatile__("cli");

    /* Discard firmware junk so the handshake starts from a clean buffer. The
     * count is a bound and not an expectation: the buffer is one byte deep and
     * a working controller clears in a read or two, but a controller that
     * reports output-full forever is a real failure on real hardware, and this
     * runs with interrupts off during boot. Unbounded, it would stop the
     * machine here with nothing said, which on a laptop with no serial port is
     * a blank refusal nobody can act on. */
    {
        int drained = PS2_DRAIN_LIMIT;

        while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            inb(PS2_DATA_PORT);
            if (--drained <= 0) {
                serial_printf("mouse: the 8042 buffer never drained\n");
                __asm__ __volatile__("sti");
                return -1;
            }
        }
    }

    if (ps2_send_command(PS2_CMD_ENABLE_MOUSE) < 0) {
        serial_printf("Mouse: failed to enable aux port\n");
        __asm__ __volatile__("sti");
        return -1;
    }

    if (ps2_send_command(PS2_CMD_READ_CONFIG) < 0) {
        serial_printf("Mouse: failed to read config\n");
        __asm__ __volatile__("sti");
        return -1;
    }
    if (ps2_wait_output() < 0) {
        serial_printf("Mouse: timeout reading config\n");
        __asm__ __volatile__("sti");
        return -1;
    }
    config = inb(PS2_DATA_PORT);

    config |= PS2_CONFIG_MOUSE_IRQ;   /* enable IRQ12 */
    config &= ~PS2_CONFIG_MOUSE_CLK;  /* clear the clock-disable bit */

    if (ps2_send_command(PS2_CMD_WRITE_CONFIG) < 0) {
        serial_printf("Mouse: failed to write config cmd\n");
        __asm__ __volatile__("sti");
        return -1;
    }
    if (ps2_wait_input() < 0) {
        serial_printf("Mouse: timeout writing config\n");
        __asm__ __volatile__("sti");
        return -1;
    }
    outb(PS2_DATA_PORT, config);

    response = mouse_send_command(MOUSE_CMD_RESET);
    if (response < 0) {
        serial_printf("Mouse: reset failed\n");
    } else {
        /* Reset emits 0xAA (self-test pass) then 0x00 (device ID). */
        if (ps2_wait_output() == 0) {
            inb(PS2_DATA_PORT);  /* 0xAA */
        }
        if (ps2_wait_output() == 0) {
            inb(PS2_DATA_PORT);  /* 0x00 */
        }
    }

    response = mouse_send_command(MOUSE_CMD_SET_DEFAULTS);
    if (response < 0) {
        serial_printf("Mouse: set defaults failed\n");
    }

    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        serial_printf("Mouse: enable failed\n");
        __asm__ __volatile__("sti");
        return -1;
    }

    register_isr_handler(44, mouse_irq_handler);

    mouse_initialized = true;

    __asm__ __volatile__("sti");

    return 0;
}

/**
 * Drain up to max_count packets from the ring buffer into buf.
 *
 * The consumer side of the buffer that mouse_irq_handler fills. It never
 * blocks: it copies whatever packets are present and returns the count,
 * which may be zero.
 *
 * @return the number of packets copied.
 */
int mouse_read_packets(struct mouse_packet *buf, int max_count)
{
    int count = 0;

    while (count < max_count && mouse_head != mouse_tail) {
        buf[count++] = mouse_buf[mouse_head];
        mouse_head = (mouse_head + 1) % MOUSE_BUF_SIZE;
    }

    return count;
}

void mouse_clear_packets(void)
{
    mouse_head = 0;
    mouse_tail = 0;
    packet_index = 0;
}

bool mouse_is_available(void)
{
    return mouse_initialized;
}
