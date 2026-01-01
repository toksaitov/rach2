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

/*
 * Wait for 8042 input buffer to be empty (ready to receive command).
 * Returns 0 on success, -1 on timeout.
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

/*
 * Wait for 8042 output buffer to have data (ready to read).
 * Returns 0 on success, -1 on timeout.
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

/*
 * Send a command to the 8042 controller.
 */
static int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_input() < 0) return -1;
    outb(PS2_CMD_PORT, cmd);
    return 0;
}

/*
 * Send a command to the mouse (via the 8042 controller).
 * Returns the mouse's response byte, or -1 on error.
 */
static int mouse_send_command(uint8_t cmd)
{
    int retries = 3;

    while (retries-- > 0) {
        /* Tell 8042 to send next byte to mouse */
        if (ps2_send_command(PS2_CMD_WRITE_MOUSE) < 0) {
            continue;
        }

        /* Send the actual command byte */
        if (ps2_wait_input() < 0) {
            continue;
        }
        outb(PS2_DATA_PORT, cmd);

        /* Wait for and read response */
        if (ps2_wait_output() < 0) {
            continue;
        }

        uint8_t response = inb(PS2_DATA_PORT);

        if (response == MOUSE_ACK) {
            return response;
        } else if (response == MOUSE_RESEND) {
            /* Mouse wants us to resend, try again */
            continue;
        } else {
            /* Unexpected response, but might still be OK for some commands */
            return response;
        }
    }

    return -1;
}

/*
 * Push a completed mouse packet to the ring buffer.
 */
static void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    int next = (mouse_tail + 1) % MOUSE_BUF_SIZE;
    if (next != mouse_head) {
        /* Clamp deltas to int8_t range */
        if (dx > 127) dx = 127;
        if (dx < -128) dx = -128;
        if (dy > 127) dy = 127;
        if (dy < -128) dy = -128;

        mouse_buf[mouse_tail].buttons = buttons & 0x07;  /* Only bits 0-2 */
        mouse_buf[mouse_tail].dx = (int8_t)dx;
        mouse_buf[mouse_tail].dy = (int8_t)dy;
        mouse_tail = next;
    }
}

/*
 * IRQ12 handler - called when mouse sends data.
 */
static void mouse_irq_handler(isr_params_t *params)
{
    (void)params;

    uint8_t status = inb(PS2_STATUS_PORT);

    /* Check if data is from mouse (bit 5 set) and output buffer full */
    if (!(status & PS2_STATUS_OUTPUT_FULL)) {
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    /* Check if this is mouse data (status bit 5) */
    if (!(status & PS2_STATUS_MOUSE_DATA)) {
        /* This is keyboard data, ignore (keyboard handler will get it) */
        return;
    }

    /*
     * Packet synchronization:
     * Byte 0 always has bit 3 set (the "always 1" bit).
     * If we're at index 0 and bit 3 is not set, we're out of sync.
     */
    if (packet_index == 0) {
        if (!(data & 0x08)) {
            /* Out of sync, skip this byte */
            return;
        }
    }

    packet_bytes[packet_index++] = data;

    if (packet_index >= 3) {
        /* Complete packet received */
        uint8_t flags = packet_bytes[0];
        int dx = packet_bytes[1];
        int dy = packet_bytes[2];

        /* Sign-extend using the sign bits in flags */
        if (flags & 0x10) {  /* X sign bit */
            dx |= 0xFFFFFF00;  /* Sign extend to negative */
        }
        if (flags & 0x20) {  /* Y sign bit */
            dy |= 0xFFFFFF00;  /* Sign extend to negative */
        }

        /* Check for overflow - if overflow, discard packet */
        if (!(flags & 0xC0)) {  /* No X or Y overflow */
            /* Extract button state (bits 0-2 of flags) */
            uint8_t buttons = flags & 0x07;

            /* PS/2 Y is inverted compared to screen coordinates */
            /* Negative dy means mouse moved up (toward top of screen) */
            /* For Doom, positive dy should mean forward, so negate */
            mouse_push_packet(buttons, dx, -dy);
        }

        packet_index = 0;
    }
}

/*
 * Initialize the PS/2 mouse.
 */
int init_mouse(void)
{
    uint8_t config;
    int response;

    /* Disable interrupts during initialization */
    __asm__ __volatile__("cli");

    /* Flush any pending data from the output buffer */
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA_PORT);
    }

    /* Enable the auxiliary (mouse) port */
    if (ps2_send_command(PS2_CMD_ENABLE_MOUSE) < 0) {
        serial_printf("Mouse: failed to enable aux port\n");
        __asm__ __volatile__("sti");
        return -1;
    }

    /* Read current configuration byte */
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

    /* Enable IRQ12 and ensure mouse clock is enabled */
    config |= PS2_CONFIG_MOUSE_IRQ;   /* Enable IRQ12 */
    config &= ~PS2_CONFIG_MOUSE_CLK;  /* Enable mouse clock (clear disable bit) */

    /* Write updated configuration */
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

    /* Reset the mouse */
    response = mouse_send_command(MOUSE_CMD_RESET);
    if (response < 0) {
        serial_printf("Mouse: reset failed\n");
        /* Continue anyway, some emulators don't respond properly */
    } else {
        /* After reset, mouse sends 0xAA (self-test passed) and 0x00 (device ID) */
        /* Wait and consume these bytes */
        if (ps2_wait_output() == 0) {
            inb(PS2_DATA_PORT);  /* 0xAA */
        }
        if (ps2_wait_output() == 0) {
            inb(PS2_DATA_PORT);  /* 0x00 */
        }
    }

    /* Set default settings */
    response = mouse_send_command(MOUSE_CMD_SET_DEFAULTS);
    if (response < 0) {
        serial_printf("Mouse: set defaults failed\n");
    }

    /* Enable data reporting */
    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        serial_printf("Mouse: enable failed\n");
        __asm__ __volatile__("sti");
        return -1;
    }

    /* Register IRQ12 handler (vector 44 = 32 + 12) */
    register_isr_handler(44, mouse_irq_handler);

    mouse_initialized = true;

    __asm__ __volatile__("sti");

    return 0;
}

/*
 * Read mouse packets from the ring buffer (non-blocking).
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

/*
 * Clear the mouse packet buffer.
 */
void mouse_clear_packets(void)
{
    mouse_head = 0;
    mouse_tail = 0;
    packet_index = 0;
}

/*
 * Check if mouse was successfully initialized.
 */
bool mouse_is_available(void)
{
    return mouse_initialized;
}
