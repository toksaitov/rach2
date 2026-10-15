/**
 * The PS/2 mouse's IRQ 12 handler, driven on the host against a model of
 * the 8042's status and data ports (sys/arch/x86/ps2mouse.c).
 *
 * The file is included whole so that the static handler and its packet
 * state are reachable; RACH2_HOSTTEST leaves out `init_mouse`, whose `cli`
 * and `sti` a host cannot run. The model's status register has the value a
 * row sets, and a read of the data port is counted and returns the row's
 * byte. A status with the output buffer full (bit 0) and bit 5 clear marks
 * a keyboard byte, which the handler must leave unread for the keyboard's
 * IRQ 1 handler, with the packet as it was. With bit 5 set the byte is the
 * mouse's: the handler reads it, and three make a packet.
 */

#include "arch/x86/ps2mouse.c"

int printf(const char *format, ...);

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

/* The model 8042: the status the row sets, the byte waiting in the data
 * port, and the count of data port reads and of any port write. */
static uint8_t model_status;
static uint8_t model_data;
static int data_reads;
static int port_writes;

/* The packets the handler handed to the mouse queue. */
static int pushes;
static uint8_t pushed_buttons;
static int pushed_dx;
static int pushed_dy;

uint8_t inb(uint16_t port)
{
    if (port == PS2_STATUS_PORT) {
        return model_status;
    }
    if (port == PS2_DATA_PORT) {
        data_reads++;
        return model_data;
    }
    return 0xFF;
}

void outb(uint16_t port, uint8_t val)
{
    (void)port;
    (void)val;
    port_writes++;
}

void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    pushes++;
    pushed_buttons = buttons;
    pushed_dx = dx;
    pushed_dy = dy;
}

/** Run one IRQ 12 with `status` in the status register and `data` waiting,
 * and return the number of times it read the data port. */
static int interrupt(uint8_t status, uint8_t data)
{
    int before = data_reads;

    model_status = status;
    model_data = data;
    mouse_irq_handler(0);
    return data_reads - before;
}

int main(void)
{
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

    /* The rest of the packet: X is -2 and Y, positive upward, is -3. */
    CHECK("byte 1 is read", interrupt(MOUSE_BYTE, 0xFE) == 1);
    CHECK("byte 2 is read", interrupt(MOUSE_BYTE, 0xFD) == 1);
    CHECK("three mouse bytes make one packet", pushes == 1);
    CHECK("the packet starts the next at byte 0", packet_index == 0);
    CHECK("the packet has the left button", pushed_buttons == 0x01);
    CHECK("the packet's dx is -2", pushed_dx == -2);
    CHECK("the packet's dy is 3, Y negated", pushed_dy == 3);

    CHECK("the handler writes no port", port_writes == 0);

    if (fails == 0) {
        printf("ps2mouse-test: pass\n");
    }
    return fails ? 1 : 0;
}
