/**
 * The PS/2 mouse's IRQ 12 handler and its commands, driven on the host
 * against a model of the 8042's status and data ports
 * (sys/arch/x86/ps2mouse.c).
 *
 * The file is included whole so that the static functions and the packet
 * state are reachable; RACH2_HOSTTEST leaves out `init_mouse`, whose `cli`
 * and `sti` a host cannot run. The model's output buffer is a queue of the
 * bytes a row puts there, each with its status: the status register reads
 * as the first one's, and a read of the data port is counted and takes it.
 * A status with the output buffer full (bit 0) and bit 5 clear marks a
 * keyboard byte, which the handler must leave unread for the keyboard's
 * IRQ 1 handler, with the packet as it was. With bit 5 set the byte is the
 * mouse's: the handler reads it, and three make a packet. A command to the
 * mouse takes as its reply only a byte marked as the mouse's, and drops a
 * keyboard byte that comes first.
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

/* The model 8042: the bytes waiting in its output buffer, first at
 * `model_head`, and the count of data port reads and of port writes, with
 * the last two writes. */
#define MODEL_BYTES 8
static uint8_t model_status[MODEL_BYTES];
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;
static int data_reads;
static int port_writes;
static uint16_t write_port[2];
static uint8_t write_value[2];

/* The packets the handler handed to the mouse queue. */
static int pushes;
static uint8_t pushed_buttons;
static int pushed_dx;
static int pushed_dy;

uint8_t inb(uint16_t port)
{
    uint8_t data;

    if (port == PS2_STATUS_PORT) {
        return model_count > 0 ? model_status[model_head] : 0;
    }
    if (port == PS2_DATA_PORT) {
        data_reads++;
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
    write_port[0] = write_port[1];
    write_value[0] = write_value[1];
    write_port[1] = port;
    write_value[1] = val;
    port_writes++;
}

/** Empty the output buffer. */
static void model_clear(void)
{
    model_head = 0;
    model_count = 0;
}

/** Put `data` in the output buffer behind what waits there, with `status`. */
static void model_put(uint8_t status, uint8_t data)
{
    model_status[model_head + model_count] = status;
    model_data[model_head + model_count] = data;
    model_count++;
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

    model_clear();
    model_put(status, data);
    mouse_irq_handler(0);
    model_clear();
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

    /* The rest of the packet: X is -2 and Y, positive away from the user,
     * is -3. */
    CHECK("byte 1 is read", interrupt(MOUSE_BYTE, 0xFE) == 1);
    CHECK("byte 2 is read", interrupt(MOUSE_BYTE, 0xFD) == 1);
    CHECK("three mouse bytes make one packet", pushes == 1);
    CHECK("the packet starts the next at byte 0", packet_index == 0);
    CHECK("the packet has the left button", pushed_buttons == 0x01);
    CHECK("the packet's dx is -2", pushed_dx == -2);
    CHECK("the packet's dy is -3, as the wire gives it", pushed_dy == -3);

    CHECK("the handler writes no port", port_writes == 0);

    /* A key pressed before the mouse acknowledges a command. */
    model_clear();
    model_put(KEYBOARD_BYTE, 0x1C);
    model_put(MOUSE_BYTE, MOUSE_ACK);
    CHECK("a key before the ACK is not the reply",
          mouse_send_command(MOUSE_CMD_ENABLE) == MOUSE_ACK);
    CHECK("the key and the ACK are both read", model_count == 0);
    CHECK("the command goes to the mouse",
          write_port[0] == PS2_CMD_PORT && write_value[0] == PS2_CMD_WRITE_MOUSE &&
              write_port[1] == PS2_DATA_PORT && write_value[1] == MOUSE_CMD_ENABLE);

    /* A key and nothing from the mouse: three tries, each timed out. */
    model_clear();
    model_put(KEYBOARD_BYTE, 0x1C);
    port_writes = 0;
    CHECK("a key alone is no reply", mouse_send_command(MOUSE_CMD_ENABLE) == -1);
    CHECK("the command is tried three times", port_writes == 6);

    /* Keys pressed between the bytes of the reset's reply. */
    model_clear();
    model_put(MOUSE_BYTE, MOUSE_ACK);
    model_put(KEYBOARD_BYTE, 0x1E);
    model_put(MOUSE_BYTE, 0xAA);
    model_put(KEYBOARD_BYTE, 0x9E);
    model_put(MOUSE_BYTE, 0x00);
    CHECK("the reset is answered", mouse_reset() == 0);
    CHECK("the reset's reply leaves no byte waiting", model_count == 0);

    if (fails == 0) {
        printf("ps2mouse-test: pass\n");
    }
    return fails ? 1 : 0;
}
