/*
 * i8042.c: the waits and writes of the 8042 controller, shared by the PS/2
 * keyboard and mouse, as i8042.h describes them.
 */

#include "i8042.h"
#include "portio.h"

int ps2_wait_input(void)
{
    int timeout = PS2_TIMEOUT;

    while (timeout-- > 0) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

int ps2_wait_output(void)
{
    int timeout = PS2_TIMEOUT;

    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return 0;
        }
    }
    return -1;
}

int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_input() < 0) {
        return -1;
    }
    outb(PS2_CMD_PORT, cmd);
    return 0;
}

int ps2_kbd_write(uint8_t byte)
{
    uint8_t status = inb(PS2_STATUS_PORT);

    if (status == 0xFF || (status & PS2_STATUS_INPUT_FULL) != 0) {
        return -1;
    }
    outb(PS2_DATA_PORT, byte);
    return 0;
}
