#ifndef _I8042_H_
#define _I8042_H_

/*
 * i8042.h: the 8042 keyboard and mouse controller's ports and the waits
 * around a write, which the keyboard's driver (ps2kbd.c) and the mouse's
 * (ps2mouse.c) share.
 */

#include "../../lib/kint.h"

/* The data port, where a byte from either device waits and a byte for the
 * keyboard goes; the status port, read; the command port, the same address
 * written. */
#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

/* The status register's bits. A controller that is not there reads 0xFF. */
#define PS2_STATUS_OUTPUT_FULL 0x01 /* a byte waits in the data port */
#define PS2_STATUS_INPUT_FULL  0x02 /* the controller has not taken the last write */
#define PS2_STATUS_MOUSE_DATA  0x20 /* the waiting byte is the mouse's */

/* How many reads of the status a wait makes before it gives up. */
#define PS2_TIMEOUT 100000

/**
 * Spin until the controller's input buffer is empty, ready for a byte.
 * Returns 0 when it is, -1 after PS2_TIMEOUT reads of the status. For the
 * bring-up, which runs with interrupts off and may wait.
 */
int ps2_wait_input(void);

/**
 * Spin until the controller's output buffer has a byte, of either device.
 * Returns 0 when it has, -1 after PS2_TIMEOUT reads of the status.
 */
int ps2_wait_output(void);

/** Write `cmd` to the controller's command port once its input buffer is
 * empty. Returns 0, or -1 when the buffer never empties. */
int ps2_send_command(uint8_t cmd);

/**
 * Write `byte` to the keyboard, on the controller's first port, only when
 * the input buffer is empty now: it does not wait, so a timer or interrupt
 * handler may call it. Returns 0 when the byte went out, and -1 when the
 * buffer was full or no controller answers, for the caller to try again.
 */
int ps2_kbd_write(uint8_t byte);

#endif /* _I8042_H_ */
