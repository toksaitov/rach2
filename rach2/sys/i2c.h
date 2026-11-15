/*
 * i2c.h: I2C adapters, after Linux's i2c-core: a driver's buses by number,
 * each with its directory in /sys and its /dev/i2c-N, Linux's i2c-dev, by
 * which a program runs I2C and SMBus transfers on the bus.
 */
#ifndef _I2C_H_
#define _I2C_H_

#include "lib/kint.h"
#include "fs/devfs.h"

/* The flags of a message, Linux's I2C_M_; an adapter honours I2C_M_RD and
 * ignores the protocol's variations, as GMBUS does on Linux. */
#define I2C_M_RD           0x0001u
#define I2C_M_TEN          0x0010u
#define I2C_M_DMA_SAFE     0x0200u
#define I2C_M_RECV_LEN     0x0400u
#define I2C_M_NO_RD_ACK    0x0800u
#define I2C_M_IGNORE_NAK   0x1000u
#define I2C_M_REV_DIR_ADDR 0x2000u
#define I2C_M_NOSTART      0x4000u
#define I2C_M_STOP         0x8000u

/* What an adapter does, Linux's I2C_FUNC_. */
#define I2C_FUNC_I2C                    0x00000001u
#define I2C_FUNC_10BIT_ADDR             0x00000002u
#define I2C_FUNC_SMBUS_PEC              0x00000008u
#define I2C_FUNC_SMBUS_QUICK            0x00010000u
#define I2C_FUNC_SMBUS_READ_BYTE        0x00020000u
#define I2C_FUNC_SMBUS_WRITE_BYTE       0x00040000u
#define I2C_FUNC_SMBUS_READ_BYTE_DATA   0x00080000u
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA  0x00100000u
#define I2C_FUNC_SMBUS_READ_WORD_DATA   0x00200000u
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA  0x00400000u
#define I2C_FUNC_SMBUS_PROC_CALL        0x00800000u
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x01000000u
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000u
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK   0x04000000u
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK  0x08000000u

/* Linux's I2C_FUNC_SMBUS_EMUL: the SMBus transfers the core makes of plain
 * I2C messages. */
#define I2C_FUNC_SMBUS_EMUL                                                                      \
    (I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE |               \
     I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA |                            \
     I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA | I2C_FUNC_SMBUS_PROC_CALL | \
     I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | I2C_FUNC_SMBUS_READ_I2C_BLOCK |                           \
     I2C_FUNC_SMBUS_WRITE_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)

/* The most messages of one I2C_RDWR and bytes of one message, Linux's
 * I2C_RDWR_IOCTL_MAX_MSGS and i2c-dev's 8192. */
#define I2C_RDWR_MAX_MSGS 42u
#define I2C_MSG_MAX_BYTES 8192u

/* The most adapters at once, i2c-0 to i2c-31. */
#define I2C_ADAPTERS_MAX 32

/*
 * A message in the kernel, Linux's struct i2c_msg: `len` bytes of `buf`
 * to or from the device at `addr`, with I2C_M_ `flags`.
 */
struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};

/*
 * An adapter, Linux's i2c_adapter. The driver sets the first five members
 * before `i2c_add_adapter`; the core owns the rest.
 *
 * `xfer` runs the `n` messages as one transaction, a repeated start between
 * them and a stop at the end, in process context with no lock of the
 * core's held: `n`, or -errno, -ENXIO when no device answered its address
 * and -ENODEV for a device gone. It may sleep. `hold` keeps the driver's
 * record while an open file of /dev may still call `xfer`: called with 1
 * at an open and 0 at its close; NULL for a record that outlives the
 * adapter. `func` is the adapter's I2C_FUNC_ bits, and `name` its name in
 * /sys.
 */
struct i2c_adapter {
    int32_t (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int n);
    void (*hold)(struct i2c_adapter *adap, int get);
    void *priv;
    uint32_t func;
    char name[48];

    int nr;              /* N of i2c-N, or -1 while not added */
    uint8_t gone;        /* deleted; an open file's transfers are -ENODEV */
    int32_t retries;     /* I2C_RETRIES: tries again of an -EAGAIN */
    uint32_t timeout_ms; /* I2C_TIMEOUT: how long the tries may take */
    int dir, dev_dir;    /* /sys handles of i2c-N and i2c-dev/i2c-N, or 0 */
    int links[3];        /* bus, class and /sys/dev/char links, or 0 */
    char devname[8];     /* "i2c-N" */
    struct devfs_node node;
};

/**
 * Add `adap` at the lowest free number, as Linux's i2c_add_adapter and
 * i2c-dev's notifier: its directory i2c-N below `parent` in /sys with
 * `name`, `uevent` and `subsystem`, its link in /sys/bus/i2c/devices, and
 * /dev/i2c-N with its class device below the adapter's, its links in
 * /sys/class/i2c-dev and /sys/dev/char, and /sys/module/i2c_dev, made on
 * the first add, then their uevents `add`. Process context. 0, or -errno
 * with nothing left made.
 */
int32_t i2c_add_adapter(struct i2c_adapter *adap, int parent);

/** Take `adap` away, Linux's i2c_del_adapter: its node and directories go,
 * and an open file's later transfers are -ENODEV. Nothing for an adapter
 * not added. Process context. */
void i2c_del_adapter(struct i2c_adapter *adap);

/**
 * Run `n` messages on `adap`, Linux's i2c_transfer: tried again on -EAGAIN
 * as I2C_RETRIES and I2C_TIMEOUT allow. `n`, or -errno, -ENODEV once the
 * adapter is deleted. Process context; may sleep.
 */
int32_t i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int n);

#endif /* _I2C_H_ */
