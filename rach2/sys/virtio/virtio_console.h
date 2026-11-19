/*
 * virtio_console.h: the virtio console (virtio 1.2, 5.3), driven as Linux's
 * drivers/char/virtio_console.c drives it with its hvc console: the device's
 * layout, and the driver's calls for the boot, the transport and /dev.
 */

#ifndef _VIRTIO_CONSOLE_H_
#define _VIRTIO_CONSOLE_H_

#include "virtio.h"

/* The device's feature bits (virtio 1.2, 5.3.3). */
#define VIRTIO_CONSOLE_F_SIZE        0 /* the configuration has the console's size */
#define VIRTIO_CONSOLE_F_MULTIPORT   1 /* several ports, and the control queues */
#define VIRTIO_CONSOLE_F_EMERG_WRITE 2 /* a byte written to `emerg_wr` is output */

/* The offsets of the device's configuration (virtio 1.2, 5.3.4): `cols`
 * and `rows`, 16 bits each, valid with F_SIZE; `max_nr_ports`, 32 bits,
 * valid with F_MULTIPORT; `emerg_wr`, 32 bits, valid with F_EMERG_WRITE. */
#define VIRTIO_CONSOLE_CFG_COLS      0u
#define VIRTIO_CONSOLE_CFG_ROWS      2u
#define VIRTIO_CONSOLE_CFG_MAX_PORTS 4u
#define VIRTIO_CONSOLE_CFG_EMERG_WR  8u

/* A message of the control queues (virtio 1.2, 5.3.6.2). PORT_NAME's name
 * and RESIZE's size follow it in the same buffer. */
struct virtio_console_control {
    uint32_t id;    /* the port, or VIRTIO_CONSOLE_BAD_ID for the device */
    uint16_t event; /* VIRTIO_CONSOLE_* below */
    uint16_t value;
};

#define VIRTIO_CONSOLE_DEVICE_READY 0 /* driver: the control queues are up */
#define VIRTIO_CONSOLE_PORT_ADD     1 /* device: the port exists */
#define VIRTIO_CONSOLE_PORT_REMOVE  2 /* device: the port has gone */
#define VIRTIO_CONSOLE_PORT_READY   3 /* driver: the port is set up */
#define VIRTIO_CONSOLE_CONSOLE_PORT 4 /* device: the port is a console */
#define VIRTIO_CONSOLE_RESIZE       5 /* device: a console port's size follows */
#define VIRTIO_CONSOLE_PORT_OPEN    6 /* either: the side's end opened or closed */
#define VIRTIO_CONSOLE_PORT_NAME    7 /* device: the port's name follows */

#define VIRTIO_CONSOLE_BAD_ID 0xFFFFFFFFu

/* The ports the driver takes, 0 and 1: port 0's queues are 0 and 1, the
 * control queues 2 and 3, and port 1's 4 and 5 (virtio 1.2, 5.3.2). */
#define VIRTIO_CONSOLE_PORTS 2

/* What the driver counts, for a test and for the log. */
struct virtio_console_counters {
    uint32_t rx_bytes;    /* bytes the line's port received and the console took */
    uint32_t rx_dropped;  /* bytes received on a port that is not the console's input */
    uint32_t rx_held;     /* times a full console held a received buffer back */
    uint32_t tx_bytes;    /* bytes given to the device */
    uint32_t tx_buffers;  /* buffers they went in */
    uint32_t tx_dropped;  /* bytes dropped: no line, or a device that stopped taking */
    uint32_t tx_waits;    /* waits for the device that reached their bound */
    uint32_t tx_panic;    /* bytes a panic gave through a port's own buffer */
    uint32_t emerg;       /* bytes written to `emerg_wr` during a panic */
    uint32_t control_in;  /* control messages received */
    uint32_t control_out; /* control messages sent */
};

/**
 * Find the first virtio console of virtio-pci, bind it
 * (`virtio_console_bind`) and register the driver for one bound later
 * through /sys. Called once at boot, after `virtio_pci_probe`.
 *
 * The boot line's `console=` words choose the console's lines, each in
 * turn, as Linux's do: `hvc0` is the device's first console port,
 * `vport0pN` its port N whatever its kind, and `ttyS0` the UART. The log
 * goes to every line named and the console's input comes from the last.
 * With no `console=` that names a port the console stays on the UART and
 * the device is bound with no line. A device with no interrupt is refused.
 * With `rach2.vconpanic` on the boot line it then runs
 * `virtio_console_panic_test`.
 *
 * @return 1 when a device was bound, 0 when there is none, or -errno.
 */
int32_t virtio_console_start(void);

/**
 * The driver's probe of a console device, as Linux's virtcons_probe: its
 * features negotiated (F_SIZE, F_MULTIPORT, F_EMERG_WRITE), its queues set
 * up, the device shown bound to `virtio_console`, and with F_MULTIPORT the
 * device told the driver is ready, on which it announces its ports. A port
 * the boot line names becomes the console's line as soon as it is known.
 *
 * @return 0; -EBUSY for a second console, which the driver does not take;
 *         or the error of the bring-up, with the device reset. The device
 *         is given back with virtio_put on an error.
 */
int32_t virtio_console_bind(struct virtio_dev *vdev);

/**
 * Stop the device at shutdown, as Linux's virtio bus resets its devices
 * there: what the ports were given is sent and waited for, the device is
 * closed to further output, control messages and attachments, the console
 * goes back to the UART, and the device is reset (`virtio_halt`), or its
 * memory access turned off when the reset does not end, so that a warm
 * restart finds no device still writing into the memory of the kernel
 * that stopped. The result is logged as `virtio_console: shutdown: NAME:`
 * `reset` or `the reset did not end`. Called once by `kernel_shutdown`,
 * after the disks are stopped; nothing without a device.
 */
void virtio_console_shutdown(void);

/**
 * The test of a panic that finds the transmit side held, by
 * `rach2.vconpanic`: take the transmit side's lock as a processor that the
 * panic stops would hold it, and panic. The panic's text must still reach
 * each port that carries the console, through the port's own panic
 * buffer. Nothing, with a record, when no port carries the console.
 */
void virtio_console_panic_test(void);

/**
 * Whether /dev/NAME is the node of the port that is the console's input
 * now: `hvcN` for a console port, N the number it was given when it became
 * one, `vport0pN` for port N of another kind.
 * devfs shows the node while this holds, and an open of it gives the
 * console terminal, as /dev/console does.
 */
int virtio_console_node(const char *name);

/** Copy the driver's counters out. */
void virtio_console_counters(struct virtio_console_counters *out);

#endif /* _VIRTIO_CONSOLE_H_ */
