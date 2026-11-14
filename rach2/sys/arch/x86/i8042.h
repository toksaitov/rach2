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

/* The controller's commands that read and write its configuration byte,
 * which follows on the data port, and the byte's bit that has the
 * controller translate the keyboard's set 2 into set 1. */
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CONFIG_TRANSLATE 0x40

/* The status register's bits. A controller that is not there reads 0xFF. */
#define PS2_STATUS_OUTPUT_FULL 0x01 /* a byte waits in the data port */
#define PS2_STATUS_INPUT_FULL  0x02 /* the controller has not taken the last write */
#define PS2_STATUS_MOUSE_DATA  0x20 /* the waiting byte is the mouse's */

/* How many reads of the status a wait makes before it gives up, and how long
 * it lasts at most by the clock, Linux's 10000 waits of 50 us
 * (I8042_CTL_TIMEOUT): under a hypervisor each read is an exit, and
 * PS2_TIMEOUT of them took 42 s at the third level. */
#define PS2_TIMEOUT    100000
#define PS2_TIMEOUT_US 500000u

/** The clock at the start of a wait of the status, for `ps2_wait_over`. */
uint32_t ps2_wait_begin(void);

/**
 * Whether a wait of the status begun at `start` (`ps2_wait_begin`) is over
 * after `reads` reads: PS2_TIMEOUT of them, or PS2_TIMEOUT_US by the clock,
 * which is read every 64 reads. A host test's model has no clock and ends by
 * the reads alone.
 */
int ps2_wait_over(uint32_t start, int reads);

/**
 * Spin until the controller's input buffer is empty, ready for a byte.
 * Returns 0 when it is, -1 after PS2_TIMEOUT reads of the status or
 * PS2_TIMEOUT_US. For the bring-up, which runs with interrupts off and may
 * wait.
 */
int ps2_wait_input(void);

/**
 * Spin until the controller's output buffer has a byte, of either device.
 * Returns 0 when it has, -1 after PS2_TIMEOUT reads of the status or
 * PS2_TIMEOUT_US.
 */
int ps2_wait_output(void);

/** Write `cmd` to the controller's command port once its input buffer is
 * empty. Returns 0, or -1 when the buffer never empties. */
int ps2_send_command(uint8_t cmd);

/**
 * Write `config` as the controller's configuration byte: WRITE_CONFIG, and
 * the byte once the controller has taken the command. Returns 0, or -1
 * when the controller does not take the command or the byte in time. A
 * controller that takes the command late takes the next byte written to
 * the data port for its configuration, so a caller in the controller's
 * setup fails the setup (`i8042_fail`) and writes nothing more, as Linux's
 * i8042_probe fails. The caller keeps interrupts off.
 */
int ps2_write_config(uint8_t config);

/**
 * Read the controller's configuration byte as Linux's i8042_controller_init
 * reads it: the bytes waiting in the output buffer drained first, at most 16
 * of them, then READ_CONFIG until two reads in a row agree, 50 us apart and
 * at most 10 reads. A key or mouse byte that arrives after the drain answers
 * one read and leaves the controller's byte for the next, so those two
 * reads disagree and the reads go on; the last READ_CONFIG's answer is then
 * still to come. The agreed byte is written back, as Linux's setup writes
 * its CTR, and once the controller has taken it, and so has answered every
 * READ_CONFIG, the bytes left are drained again, as Linux's i8042_flush ends
 * its setup, so that no handler takes one for a device's byte.
 * Returns the byte, or -1 when no controller answers, a read times out, no
 * two reads agree or the write-back times out: then the controller's setup
 * has failed (`i8042_failed`), logged with Linux's messages, and neither
 * port may be bound, as Linux's i8042_probe fails with EIO. Returns -1
 * without touching the controller once its setup has failed. The caller
 * keeps interrupts off, so that no handler takes the byte.
 */
int ps2_read_config(void);

/** Mark the controller's setup failed, as `ps2_read_config` does, for a
 * timeout of a configuration read or write in a port's setup. */
void i8042_fail(void);

/** Whether the controller's setup has failed: then no port is bound, IRQ
 * 1 and IRQ 12 stay masked, and nothing more is written to it. */
int i8042_failed(void);

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
