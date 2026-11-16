/*
 * i2c.c: I2C adapters and /dev/i2c-N, after Linux 6.12's i2c-core-base.c,
 * the SMBus emulation of i2c-core-smbus.c, and i2c-dev.c.
 */

#include "i2c.h"

#include "fs/devfs.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kstd.h"
#include "lib/kstring.h"
#include "mm/kheap.h"
#include "mm/usercopy.h"
#include "timer.h"

/* Linux's I2C_MAJOR, whose minor is the adapter's number. */
#define I2C_MAJOR 89

/* The requests of <linux/i2c-dev.h>, numbered without the size encoding. */
#define I2C_RETRIES     0x0701u
#define I2C_TIMEOUT     0x0702u
#define I2C_SLAVE       0x0703u
#define I2C_TENBIT      0x0704u
#define I2C_FUNCS       0x0705u
#define I2C_SLAVE_FORCE 0x0706u
#define I2C_RDWR        0x0707u
#define I2C_PEC         0x0708u
#define I2C_SMBUS       0x0720u

/* A client's flag beside I2C_M_TEN: append the SMBus packet error code. */
#define I2C_CLIENT_PEC 0x04u

/* SMBus transfers, Linux's I2C_SMBUS_ sizes and directions. */
#define SMBUS_WRITE           0
#define SMBUS_READ            1
#define SMBUS_QUICK           0u
#define SMBUS_BYTE            1u
#define SMBUS_BYTE_DATA       2u
#define SMBUS_WORD_DATA       3u
#define SMBUS_PROC_CALL       4u
#define SMBUS_BLOCK_DATA      5u
#define SMBUS_I2C_BLOCK_BROKE 6u
#define SMBUS_BLOCK_PROC_CALL 7u
#define SMBUS_I2C_BLOCK_DATA  8u
#define SMBUS_BLOCK_MAX       32u

/* The default bound of a transfer's tries, Linux's HZ of adap->timeout. */
#define I2C_TIMEOUT_MS 1000u

/* The sysfs attributes, by `arg`. */
#define ATTR_NAME          0
#define ATTR_ADAPTER_EVENT 1
#define ATTR_DEV           2
#define ATTR_DEV_EVENT     3
#define ATTR_INITSTATE     4

/* Linux's union i2c_smbus_data: a byte, a word, or a block whose first
 * byte is its length. */
union smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[SMBUS_BLOCK_MAX + 2];
};

/* An open /dev/i2c-N, Linux's i2c_client of i2c-dev: the address and flags
 * that read, write and I2C_SMBUS use. */
struct i2c_client {
    struct i2c_adapter *adap;
    uint16_t addr;
    uint16_t flags;
};

static struct i2c_adapter *adapters[I2C_ADAPTERS_MAX];

/* /sys/bus/i2c, its `devices`, /sys/class/i2c-dev and /sys/module/i2c_dev,
 * made at the first add and kept, as the built-in core and i2c-dev stay. */
static int bus_dir, bus_devices, dev_class, module_made;

/* ------------------------------------------------------------------------
 * Transfers
 */

int32_t i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int n)
{
    uint64_t start = tick_count();
    uint32_t limit = adap->timeout_ms ? adap->timeout_ms : I2C_TIMEOUT_MS;
    int32_t ret = -EAGAIN, tries;

    for (tries = 0; tries <= adap->retries && ret == -EAGAIN; tries++) {
        if (adap->gone) {
            return -ENODEV;
        }
        ret = adap->xfer(adap, msgs, n);
        if (tick_msecs(tick_count() - start) > limit) {
            break;
        }
    }
    return ret;
}

/** One step of Linux's SMBus PEC, a CRC-8 of polynomial x^8+x^2+x+1. */
static uint8_t crc8(uint16_t data)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (data & 0x8000u) {
            data ^= 0x1070u << 3;
        }
        data = (uint16_t)(data << 1);
    }
    return (uint8_t)(data >> 8);
}

/** The PEC of `msg`, its address byte first, continued from `pec`, Linux's
 * i2c_smbus_msg_pec. */
static uint8_t msg_pec(uint8_t pec, const struct i2c_msg *msg)
{
    uint8_t addr = (uint8_t)((msg->addr << 1) | (msg->flags & I2C_M_RD ? 1u : 0u));
    uint32_t i;

    pec = crc8((uint16_t)((pec ^ addr) << 8));
    for (i = 0; i < msg->len; i++) {
        pec = crc8((uint16_t)((pec ^ msg->buf[i]) << 8));
    }
    return pec;
}

/** The two messages of an SMBus transfer, their buffers, and what the
 * emulation decided about them. */
struct smbus_msgs {
    struct i2c_msg msg[2];
    uint8_t buf0[SMBUS_BLOCK_MAX + 3];
    uint8_t buf1[SMBUS_BLOCK_MAX + 2];
    int n;
    int read;
};

/**
 * Lay out the messages of SMBus transfer `size` with command `command`,
 * Linux's i2c_smbus_xfer_emulated up to its transfer. 0, -EINVAL for a
 * block too long, or -EOPNOTSUPP for the block reads, whose length comes
 * from the device and needs I2C_M_RECV_LEN, which no adapter here does.
 */
static int32_t smbus_layout(struct smbus_msgs *s, uint32_t size, uint8_t command,
                            const union smbus_data *data)
{
    struct i2c_msg *m = s->msg;

    m[0].len = 1;
    m[1].len = 0;
    s->buf0[0] = command;
    switch (size) {
    case SMBUS_QUICK:
        m[0].len = 0;
        m[0].flags = (uint16_t)(m[0].flags | (s->read ? I2C_M_RD : 0u));
        s->n = 1;
        break;
    case SMBUS_BYTE:
        if (s->read) {
            m[0].flags |= I2C_M_RD;
            s->n = 1;
        }
        break;
    case SMBUS_BYTE_DATA:
        if (s->read) {
            m[1].len = 1;
        } else {
            m[0].len = 2;
            s->buf0[1] = data->byte;
        }
        break;
    case SMBUS_WORD_DATA:
    case SMBUS_PROC_CALL:
        if (size == SMBUS_PROC_CALL) {
            s->n = 2;
            s->read = 1;
        }
        if (size == SMBUS_WORD_DATA && s->read) {
            m[1].len = 2;
        } else {
            m[0].len = 3;
            m[1].len = 2;
            s->buf0[1] = (uint8_t)(data->word & 0xffu);
            s->buf0[2] = (uint8_t)(data->word >> 8);
        }
        break;
    case SMBUS_BLOCK_DATA:
        if (s->read) {
            return -EOPNOTSUPP;
        }
        if (data->block[0] > SMBUS_BLOCK_MAX) {
            return -EINVAL;
        }
        m[0].len = (uint16_t)(data->block[0] + 2u);
        k_memcpy(s->buf0 + 1, data->block, m[0].len - 1u);
        break;
    case SMBUS_I2C_BLOCK_DATA:
        if (data->block[0] > SMBUS_BLOCK_MAX) {
            return -EINVAL;
        }
        if (s->read) {
            m[1].len = data->block[0];
        } else {
            m[0].len = (uint16_t)(data->block[0] + 1u);
            k_memcpy(s->buf0 + 1, data->block + 1, data->block[0]);
        }
        break;
    default:
        return -EOPNOTSUPP;
    }
    return 0;
}

/** Take the answer of a read out of `s` into `data`, Linux's tail of
 * i2c_smbus_xfer_emulated. */
static void smbus_answer(const struct smbus_msgs *s, uint32_t size, union smbus_data *data)
{
    switch (size) {
    case SMBUS_BYTE:
        data->byte = s->buf0[0];
        break;
    case SMBUS_BYTE_DATA:
        data->byte = s->buf1[0];
        break;
    case SMBUS_WORD_DATA:
    case SMBUS_PROC_CALL:
        data->word = (uint16_t)(s->buf1[0] | (s->buf1[1] << 8));
        break;
    case SMBUS_I2C_BLOCK_DATA:
        k_memcpy(data->block + 1, s->buf1, data->block[0]);
        break;
    default:
        break;
    }
}

/**
 * An SMBus transfer made of I2C messages to `c`'s address, Linux's
 * i2c_smbus_xfer with its emulation: `read` SMBUS_READ or SMBUS_WRITE,
 * `size` an SMBUS_ size, the PEC added and checked when the client asks
 * for it. 0, or -errno: -EBADMSG for a PEC that does not match, -EIO for a
 * transfer cut short.
 */
static int32_t smbus_xfer(const struct i2c_client *c, int read, uint8_t command, uint32_t size,
                          union smbus_data *data)
{
    uint16_t flags = c->flags;
    struct smbus_msgs s;
    uint8_t partial = 0;
    int pec = (flags & I2C_CLIENT_PEC) && size != SMBUS_QUICK && size != SMBUS_I2C_BLOCK_DATA;
    struct i2c_msg *last;
    int32_t ret;

    flags &= I2C_M_TEN;
    k_memset(&s, 0, sizeof(s));
    s.read = read;
    s.n = read ? 2 : 1;
    s.msg[0].addr = s.msg[1].addr = c->addr;
    s.msg[0].flags = flags;
    s.msg[1].flags = (uint16_t)(flags | I2C_M_RD);
    s.msg[0].buf = s.buf0;
    s.msg[1].buf = s.buf1;
    if ((ret = smbus_layout(&s, size, command, data)) != 0) {
        return ret;
    }
    last = &s.msg[s.n - 1];
    if (pec && !(s.msg[0].flags & I2C_M_RD)) {
        if (s.n == 1) {
            s.buf0[s.msg[0].len] = msg_pec(0, &s.msg[0]);
            s.msg[0].len++;
        } else {
            partial = msg_pec(0, &s.msg[0]);
        }
    }
    if (pec && (last->flags & I2C_M_RD)) {
        last->len++;
    }
    ret = i2c_transfer(c->adap, s.msg, s.n);
    if (ret < 0) {
        return ret;
    }
    if (ret != s.n) {
        return -EIO;
    }
    if (pec && (last->flags & I2C_M_RD)) {
        last->len--;
        if (last->buf[last->len] != msg_pec(partial, last)) {
            return -EBADMSG;
        }
    }
    if (s.read) {
        smbus_answer(&s, size, data);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * /dev/i2c-N
 */

/** Bind an open to the adapter of its node, Linux's i2cdev_open: -ENODEV
 * for one deleted, -ENOMEM. */
static int32_t i2cdev_open(struct devfs_node *node, fd_entry_t *e, int flags)
{
    struct i2c_adapter *adap = (struct i2c_adapter *)node->priv;
    struct i2c_client *c;

    (void)flags;
    if (!adap || adap->gone) {
        return -ENODEV;
    }
    if (!(c = (struct i2c_client *)kzalloc(sizeof(*c)))) {
        return -ENOMEM;
    }
    c->adap = adap;
    if (adap->hold) {
        adap->hold(adap, 1);
    }
    e->priv = c;
    return 0;
}

/** Let the adapter go at the last close. */
static void i2cdev_close(fd_entry_t *e)
{
    struct i2c_client *c = (struct i2c_client *)e->priv;

    if (c) {
        if (c->adap->hold) {
            c->adap->hold(c->adap, 0);
        }
        kfree(c);
    }
    e->priv = NULL;
}

/** Read or write `count` bytes, 8192 at most, from or to the client's
 * address as one message, Linux's i2cdev_read and i2cdev_write: the bytes
 * moved, or -errno. */
static int32_t i2cdev_rw(fd_entry_t *e, char __user *ubuf, uint32_t count, int read)
{
    struct i2c_client *c = (struct i2c_client *)e->priv;
    struct i2c_msg msg;
    int32_t ret;

    if (count > I2C_MSG_MAX_BYTES) {
        count = I2C_MSG_MAX_BYTES;
    }
    if (!(msg.buf = (uint8_t *)kmalloc(count ? count : 1u))) {
        return -ENOMEM;
    }
    msg.addr = c->addr;
    msg.flags = (uint16_t)((c->flags & I2C_M_TEN) | (read ? I2C_M_RD : 0u));
    msg.len = (uint16_t)count;
    ret = read ? 0 : (copy_from_user(msg.buf, ubuf, count) ? -EFAULT : 0);
    if (ret == 0) {
        ret = i2c_transfer(c->adap, &msg, 1);
    }
    if (ret == 1) {
        ret = read && copy_to_user(ubuf, msg.buf, count) ? -EFAULT : (int32_t)count;
    }
    kfree(msg.buf);
    return ret;
}

/** read(2) of /dev/i2c-N (`i2cdev_rw`). */
static int32_t i2cdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return i2cdev_rw(e, buf, count, 1);
}

/** write(2) of /dev/i2c-N (`i2cdev_rw`). */
static int32_t i2cdev_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return i2cdev_rw(e, (char __user *)buf, count, 0);
}

/* The user's struct i2c_msg and struct i2c_rdwr_ioctl_data, as the
 * kernel's own ABI lays them out. */
struct user_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t __user *buf;
};

struct user_rdwr {
    struct user_msg __user *msgs;
    uint32_t nmsgs;
};

/** Free the `n` buffers of `msgs`, those not made NULL, then `msgs`. */
static void rdwr_free(struct i2c_msg *msgs, uint32_t n)
{
    while (n--) {
        kfree(msgs[n].buf);
    }
    kfree(msgs);
}

/** Copy in message `m` of the user's, Linux's check of i2cdev_ioctl_rdwr:
 * -EINVAL for one too long or an I2C_M_RECV_LEN read whose length byte
 * leaves no room for a block, -EFAULT, -ENOMEM. */
static int32_t rdwr_take(struct i2c_msg *m, const struct user_msg *u)
{
    m->addr = u->addr;
    m->flags = u->flags;
    m->len = u->len;
    if (u->len > I2C_MSG_MAX_BYTES) {
        return -EINVAL;
    }
    if (!(m->buf = (uint8_t *)kmalloc(u->len ? u->len : 1u))) {
        return -ENOMEM;
    }
    if (copy_from_user(m->buf, u->buf, u->len) != 0) {
        return -EFAULT;
    }
    if ((u->flags & I2C_M_RECV_LEN) && (!(u->flags & I2C_M_RD) || u->len < 1 || m->buf[0] < 1 ||
                                        u->len < m->buf[0] + SMBUS_BLOCK_MAX)) {
        return -EINVAL;
    }
    return 0;
}

/**
 * I2C_RDWR, Linux's i2cdev_ioctl_rdwr: the user's messages copied in, run
 * as one transaction, and the reads copied back. The messages run, or
 * -EINVAL for more than 42 or a message `rdwr_take` refuses, -EOPNOTSUPP
 * for an I2C_M_RECV_LEN read, which no adapter here does, -EFAULT,
 * -ENOMEM, or the transfer's -errno.
 */
static int32_t i2cdev_rdwr(struct i2c_client *c, uintptr_t arg)
{
    uint8_t __user *ubufs[I2C_RDWR_MAX_MSGS];
    struct user_rdwr rd;
    struct user_msg u;
    struct i2c_msg *msgs;
    int32_t ret = 0;
    uint32_t i;

    if (copy_from_user(&rd, (const void __user *)arg, sizeof(rd)) != 0) {
        return -EFAULT;
    }
    if (rd.nmsgs > I2C_RDWR_MAX_MSGS || rd.nmsgs == 0) {
        return rd.nmsgs ? -EINVAL : 0;
    }
    if (!(msgs = (struct i2c_msg *)kzalloc(rd.nmsgs * sizeof(*msgs)))) {
        return -ENOMEM;
    }
    for (i = 0; i < rd.nmsgs && ret == 0; i++) {
        ret = copy_from_user(&u, rd.msgs + i, sizeof(u)) ? -EFAULT : rdwr_take(&msgs[i], &u);
        ubufs[i] = u.buf;
        if (ret == 0 && (u.flags & I2C_M_RECV_LEN)) {
            ret = -EOPNOTSUPP;
        }
    }
    if (ret == 0) {
        ret = i2c_transfer(c->adap, msgs, (int)rd.nmsgs);
    }
    for (i = 0; ret >= 0 && i < rd.nmsgs; i++) {
        if ((msgs[i].flags & I2C_M_RD) && copy_to_user(ubufs[i], msgs[i].buf, msgs[i].len) != 0) {
            ret = -EFAULT;
        }
    }
    rdwr_free(msgs, rd.nmsgs);
    return ret;
}

/* The user's struct i2c_smbus_ioctl_data. */
struct user_smbus {
    uint8_t read_write;
    uint8_t command;
    uint32_t size;
    union smbus_data __user *data;
};

/** Whether `size` is an SMBus size I2C_SMBUS takes. */
static int smbus_size_ok(uint32_t size)
{
    return size <= SMBUS_I2C_BLOCK_DATA;
}

/**
 * I2C_SMBUS, Linux's i2cdev_ioctl_smbus: the data the transfer needs
 * copied in, the transfer, and its answer copied back. 0, or -EINVAL for a
 * bad size, direction or missing data, -EFAULT, or the transfer's -errno.
 */
static int32_t i2cdev_smbus(struct i2c_client *c, uintptr_t arg)
{
    struct user_smbus a;
    union smbus_data tmp;
    uint32_t datasize, size;
    int read;
    int32_t ret;

    if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
        return -EFAULT;
    }
    size = a.size;
    read = a.read_write == SMBUS_READ;
    if (!smbus_size_ok(size) || (a.read_write != SMBUS_READ && a.read_write != SMBUS_WRITE)) {
        return -EINVAL;
    }
    k_memset(&tmp, 0, sizeof(tmp));
    if (size == SMBUS_QUICK || (size == SMBUS_BYTE && !read)) {
        return smbus_xfer(c, read, a.command, size, &tmp);
    }
    if (!a.data) {
        return -EINVAL;
    }
    datasize = size == SMBUS_BYTE_DATA || size == SMBUS_BYTE        ? 1u
               : size == SMBUS_WORD_DATA || size == SMBUS_PROC_CALL ? 2u
                                                                    : sizeof(tmp);
    if ((size == SMBUS_PROC_CALL || size == SMBUS_BLOCK_PROC_CALL || size == SMBUS_I2C_BLOCK_DATA ||
         !read) &&
        copy_from_user(&tmp, a.data, datasize) != 0) {
        return -EFAULT;
    }
    if (size == SMBUS_I2C_BLOCK_BROKE) {
        size = SMBUS_I2C_BLOCK_DATA;
        if (read) {
            tmp.block[0] = SMBUS_BLOCK_MAX;
        }
    }
    ret = smbus_xfer(c, read, a.command, size, &tmp);
    if (ret == 0 && (size == SMBUS_PROC_CALL || size == SMBUS_BLOCK_PROC_CALL || read) &&
        copy_to_user(a.data, &tmp, datasize) != 0) {
        return -EFAULT;
    }
    return ret;
}

/**
 * The requests of /dev/i2c-N, Linux's i2cdev_ioctl: the client's address,
 * its ten-bit and PEC flags, the adapter's tries and their time, its
 * functions, and the transfers I2C_RDWR and I2C_SMBUS. -EINVAL for an
 * address or count out of range, -ENOTTY for another request.
 */
static int32_t i2cdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct i2c_client *c = (struct i2c_client *)e->priv;
    unsigned long funcs;

    switch (request) {
    case I2C_SLAVE:
    case I2C_SLAVE_FORCE:
        if (arg > (c->flags & I2C_M_TEN ? 0x3ffu : 0x7fu)) {
            return -EINVAL;
        }
        c->addr = (uint16_t)arg;
        return 0;
    case I2C_TENBIT:
        c->flags = (uint16_t)(arg ? c->flags | I2C_M_TEN : c->flags & ~I2C_M_TEN);
        return 0;
    case I2C_PEC:
        c->flags = (uint16_t)(arg ? c->flags | I2C_CLIENT_PEC : c->flags & ~I2C_CLIENT_PEC);
        return 0;
    case I2C_FUNCS:
        funcs = c->adap->func;
        return copy_to_user((void __user *)arg, &funcs, sizeof(funcs)) ? -EFAULT : 0;
    case I2C_RDWR:
        return i2cdev_rdwr(c, arg);
    case I2C_SMBUS:
        return i2cdev_smbus(c, arg);
    case I2C_RETRIES:
        if (arg > 0x7fffffffu) {
            return -EINVAL;
        }
        c->adap->retries = (int32_t)arg;
        return 0;
    case I2C_TIMEOUT:
        if (arg > 0x7fffffffu / 10u) {
            return -EINVAL;
        }
        c->adap->timeout_ms = (uint32_t)arg * 10u;
        return 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_ops i2cdev_fops = {i2cdev_read,  i2cdev_write, NULL, i2cdev_ioctl,
                                            i2cdev_close, NULL,         NULL, 0};

/* ------------------------------------------------------------------------
 * /sys
 */

/** Append `s` to `buf` at `*len`, as far as `cap` bytes leave room. */
static void emit(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    while (*s && *len < cap) {
        buf[(*len)++] = *s++;
    }
}

/** An adapter's `name`, its `uevent`, its class device's `dev` and
 * `uevent`, as Linux's i2c core and i2c-dev write them, or i2c_dev's
 * `initstate`. */
static int32_t i2c_show(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct i2c_adapter *a = (const struct i2c_adapter *)obj;
    char num[12];
    uint32_t len = 0;

    if (a) {
        (void)k_itoa(a->nr, num, sizeof(num), 10, false);
    }
    if (arg == ATTR_NAME) {
        emit(buf, cap, &len, a->name);
    } else if (arg == ATTR_ADAPTER_EVENT) {
        emit(buf, cap, &len, "DEVTYPE=i2c_adapter");
    } else if (arg == ATTR_DEV) {
        emit(buf, cap, &len, "89:");
        emit(buf, cap, &len, num);
    } else if (arg == ATTR_DEV_EVENT) {
        emit(buf, cap, &len, "MAJOR=89\nMINOR=");
        emit(buf, cap, &len, num);
        emit(buf, cap, &len, "\nDEVNAME=");
        emit(buf, cap, &len, a->devname);
    } else {
        emit(buf, cap, &len, "live");
    }
    emit(buf, cap, &len, "\n");
    return (int32_t)len;
}

/** The directory `name` below `parent`, made on its first use: its handle,
 * or -errno. */
static int i2c_dir(int parent, const char *name)
{
    int h;

    if (parent < 0) {
        return parent;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/** Make /sys/bus/i2c with `devices` and `drivers`, /sys/class/i2c-dev and
 * /sys/module/i2c_dev with `initstate` live, so that `modprobe i2c-dev`
 * finds the driver loaded; each once. 0, or -errno. */
static int32_t i2c_sysfs_init(void)
{
    static const struct sysfs_attr initstate = {"initstate", 0444u, i2c_show, NULL};
    int made, dir;

    if (bus_dir <= 0) {
        bus_dir = i2c_dir(sysfs_anchor(SYSFS_BUS, 0), "i2c");
    }
    if (bus_dir > 0 && bus_devices <= 0) {
        bus_devices = i2c_dir(bus_dir, "devices");
        (void)i2c_dir(bus_dir, "drivers");
    }
    if (dev_class <= 0) {
        dev_class = i2c_dir(sysfs_anchor(SYSFS_CLASS, 0), "i2c-dev");
    }
    if (!module_made && (dir = sysfs_module_dir("i2c_dev", &made)) > 0 &&
        sysfs_add_attr(dir, &initstate, NULL, ATTR_INITSTATE) >= 0) {
        module_made = 1;
    }
    return bus_dir < 0 ? bus_dir : bus_devices < 0 ? bus_devices : dev_class < 0 ? dev_class : 0;
}

/** Remove `adap`'s entries in /sys, after their uevents `remove` when
 * `announced` says their `add` went out. */
static void i2c_sysfs_remove(struct i2c_adapter *adap, int announced)
{
    int i;

    if (announced) {
        sysfs_uevent(adap->dev_dir, "remove", NULL);
        sysfs_uevent(adap->dir, "remove", NULL);
    }
    for (i = 0; i < 3; i++) {
        sysfs_remove(adap->links[i]);
        adap->links[i] = 0;
    }
    sysfs_remove(adap->dir);
    adap->dir = adap->dev_dir = 0;
}

/**
 * Give `adap` its directory below `parent` and its class device below
 * that, with their attributes and links, as Linux's i2c_register_adapter
 * and i2cdev_attach_adapter make them. 0, or -errno, with what was made
 * left for `i2c_sysfs_remove`.
 */
static int32_t i2c_sysfs_add(struct i2c_adapter *adap, int parent)
{
    static const struct sysfs_attr name = {"name", 0444u, i2c_show, NULL};
    static const struct sysfs_attr dev = {"dev", 0444u, i2c_show, NULL};
    char devno[16];
    int h, sub;

    if ((h = i2c_sysfs_init()) != 0 || (h = sysfs_add_dir(parent, adap->devname)) < 0) {
        return h;
    }
    adap->dir = h;
    sub = i2c_dir(adap->dir, "i2c-dev");
    if ((h = sysfs_add_attr(adap->dir, &name, adap, ATTR_NAME)) < 0 ||
        (h = sysfs_add_uevent(adap->dir, i2c_show, adap, ATTR_ADAPTER_EVENT)) < 0 ||
        (h = sysfs_add_link(adap->dir, "subsystem", bus_dir)) < 0 || (h = sub) < 0 ||
        (h = sysfs_add_dir(sub, adap->devname)) < 0) {
        return h;
    }
    adap->dev_dir = h;
    k_memcpy(devno, "89:", 3);
    (void)k_itoa(adap->nr, devno + 3, sizeof(devno) - 3, 10, false);
    if ((h = sysfs_add_attr(adap->dev_dir, &name, adap, ATTR_NAME)) < 0 ||
        (h = sysfs_add_attr(adap->dev_dir, &dev, adap, ATTR_DEV)) < 0 ||
        (h = sysfs_add_uevent(adap->dev_dir, i2c_show, adap, ATTR_DEV_EVENT)) < 0 ||
        (h = sysfs_add_link(adap->dev_dir, "subsystem", dev_class)) < 0 ||
        (h = sysfs_add_link(adap->dev_dir, "device", adap->dir)) < 0 ||
        (h = adap->links[0] = sysfs_add_link(bus_devices, adap->devname, adap->dir)) < 0 ||
        (h = adap->links[1] = sysfs_add_link(dev_class, adap->devname, adap->dev_dir)) < 0 ||
        (h = adap->links[2] =
             sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), devno, adap->dev_dir)) < 0) {
        return h;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Adapters
 */

int32_t i2c_add_adapter(struct i2c_adapter *adap, int parent)
{
    int32_t err;
    int nr;

    for (nr = 0; nr < I2C_ADAPTERS_MAX && adapters[nr]; nr++) {
    }
    if (nr == I2C_ADAPTERS_MAX) {
        return -ENOSPC;
    }
    if (parent <= 0) {
        return -ENOENT;
    }
    adap->nr = nr;
    adap->gone = 0;
    k_memcpy(adap->devname, "i2c-", 4);
    (void)k_itoa(nr, adap->devname + 4, sizeof(adap->devname) - 4, 10, false);
    k_memset(&adap->node, 0, sizeof(adap->node));
    adap->node.name = adap->devname;
    adap->node.major = I2C_MAJOR;
    adap->node.minor = (uint16_t)nr;
    adap->node.mode = 0600;
    adap->node.ops = &i2cdev_fops;
    adap->node.open = i2cdev_open;
    adap->node.priv = adap;
    if ((err = devfs_node_register(&adap->node)) != 0) {
        adap->nr = -1;
        return err;
    }
    adapters[nr] = adap;
    if ((err = i2c_sysfs_add(adap, parent)) != 0) {
        i2c_sysfs_remove(adap, 0);
        devfs_node_unregister(&adap->node);
        adapters[nr] = NULL;
        adap->nr = -1;
        return err;
    }
    sysfs_uevent(adap->dir, "add", NULL);
    sysfs_uevent(adap->dev_dir, "add", NULL);
    return 0;
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
    if (adap->nr < 0 || adap->nr >= I2C_ADAPTERS_MAX || adapters[adap->nr] != adap) {
        return;
    }
    adap->gone = 1;
    i2c_sysfs_remove(adap, 1);
    devfs_node_unregister(&adap->node);
    adapters[adap->nr] = NULL;
    adap->nr = -1;
}
