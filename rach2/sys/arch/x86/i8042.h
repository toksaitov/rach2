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

/* The 8042's ports: the keyboard's and the auxiliary device's. */
#define I8042_KBD_PORT 0
#define I8042_AUX_PORT 1

/* The most bytes of a port's firmware ID, with its NUL, as Linux's serio
 * keeps it. */
#define I8042_FIRMWARE_ID_MAX 128

/**
 * Take an ACPI device's IDs, its _HID and then each _CID, as Linux's i8042
 * PNP driver takes the PNP device that pnpacpi makes of it: only the IDs of
 * three capitals and four hexadecimal digits count, with those digits in
 * lower case and the first such ID once. When one of them is the
 * keyboard's or the PS/2 mouse's, as Linux's `pnp_kbd_devids` and
 * `pnp_aux_devids` list them, that port's firmware ID becomes "PNP:" and
 * each, as Linux's `i8042_pnp_id_to_string` writes it, the last device
 * found winning. Returns the port, or -1 when the device is neither. Called
 * while ACPICA walks its devices, in process context.
 */
int i8042_pnp_device(const char *const *ids, int n);

/** The firmware ID of `port`, I8042_*_PORT, as Linux's serio has it in
 * `firmware_id`: "PNP: LEN0036 PNP0f13" or the like, or "" when ACPI
 * named no device for it. */
const char *i8042_firmware_id(int port);

/**
 * Whether one of the IDs in the firmware ID of `port` is in `table`, which a
 * NULL ends, compared without case, as Linux's `psmouse_matches_pnp_id`:
 * 1 or 0, and 0 for a port with no firmware ID.
 */
int i8042_matches_pnp_id(int port, const char *const *table);

#endif /* _I8042_H_ */
