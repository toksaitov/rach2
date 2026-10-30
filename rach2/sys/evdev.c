/*
 * evdev.c: the input devices of /dev/input/eventN (evdev.h), with Linux's
 * evdev records, queues and ioctls.
 */

#include "evdev.h"

#include "io/poll.h"
#include "lib/kdef.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "mm/kheap.h"
#include "mm/usercopy.h"
#include "proc/wait_queue.h"
#include "rtc.h"

/*
 * A record is Linux's `struct input_event` as the caller's C library lays
 * it out, so a read copies records as they are: two longs of seconds and
 * microseconds, 16 bytes in all on i686, where musl's 64-bit time_t makes
 * Linux's header use them in place of a timeval, and 24 on x86-64.
 */
struct input_event {
    unsigned long sec;
    unsigned long usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* The records an open queues, a power of two: Linux's least, for a device
 * that gives no hint of its packets' size. */
#define EVDEV_QUEUE 64
#define QUEUE_MASK  (EVDEV_QUEUE - 1)

/*
 * An open of a device, a client in Linux's word. The records from `tail` to
 * `packet` are whole packets a read may take; those from `packet` to `head`
 * are the packet still coming. A full queue keeps only a SYN_DROPPED and
 * the newest record, as Linux's does, so that a reader that fell behind
 * learns that it lost records and starts again at a packet's end.
 */
struct evdev_client {
    struct evdev *dev;
    struct evdev_client *next; /* the device's other opens */
    unsigned head;
    unsigned tail;
    unsigned packet;
    struct input_event queue[EVDEV_QUEUE];
};

struct evdev {
    int used; /* the slot is taken, by a device or by the opens of one gone */
    int gone; /* unregistered: no node, and its opens read -ENODEV */
    int scan; /* it reports MSC_SCAN */
    const char *name;
    const char *phys;              /* NULL: EVIOCGPHYS answers -ENOENT */
    uint16_t id[4];                /* bustype, vendor, product, version */
    uint8_t keys[EVDEV_KEY_BYTES]; /* the keys it can report */
    uint8_t down[EVDEV_KEY_BYTES]; /* the keys down now, for EVIOCGKEY */
    struct evdev_client *clients;
    struct evdev_client *grab; /* the open that holds EVIOCGRAB, or NULL */
    wait_queue_t wq;           /* its readers and pollers */
};

static struct evdev devices[EVDEV_MAX];

/* Over the devices and their opens' queues. */
static struct spinlock evdev_lock = SPINLOCK_INIT;

/* The ioctl numbers are Linux's: _IOC(dir, 'E', nr, size), with the
 * direction in the top two bits, the size in the next fourteen, then the
 * type and the number. */
#define IOC_READ      2u
#define IOC_DIR(r)    ((r) >> 30)
#define IOC_SIZE(r)   (((r) >> 16) & 0x3FFFu)
#define IOC_TYPE(r)   (((r) >> 8) & 0xFFu)
#define IOC_NR(r)     ((r) & 0xFFu)
#define EVIOCGVERSION 0x80044501u
#define EVIOCGID      0x80084502u
#define EVIOCGRAB     0x40044590u
#define NR_GNAME      0x06u
#define NR_GPHYS      0x07u
#define NR_GUNIQ      0x08u
#define NR_GPROP      0x09u
#define NR_GKEY       0x18u
#define NR_GLED       0x19u
#define NR_GSND       0x1Au
#define NR_GSW        0x1Bu
#define NR_GBIT       0x20u /* to 0x3F: EVIOCGBIT(type) is 0x20 + type */
#define EV_VERSION    0x010001
#define EV_REL        0x02
#define EV_ABS        0x03
#define EV_SW         0x05
#define EV_LED        0x11
#define EV_SND        0x12
#define EV_FF         0x15
#define EV_CNT        0x20
#define REL_CNT       0x10
#define ABS_CNT       0x40
#define MSC_CNT       0x08
#define SW_CNT        0x11
#define LED_CNT       0x10
#define SND_CNT       0x08
#define FF_CNT        0x80
#define PROP_CNT      0x20
#define BITS_LONGS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define BITS_BYTES(n) (BITS_LONGS(n) * sizeof(long))

/** Whether bit `n` of the set `bits` is on. */
static int bit_on(const uint8_t *bits, unsigned n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys)
{
    struct evdev *dev = NULL;
    uint32_t flags;
    int i;

    flags = spin_lock_irqsave(&evdev_lock);
    for (i = 0; i < EVDEV_MAX; i++) {
        if (!devices[i].used) {
            dev = &devices[i];
            break;
        }
    }
    if (dev) {
        k_memset(dev, 0, sizeof(*dev));
        dev->used = 1;
        dev->name = name;
        dev->id[0] = bustype;
        dev->id[1] = vendor;
        dev->id[2] = product;
        k_memcpy(dev->keys, keys, EVDEV_KEY_BYTES);
        wait_queue_init(&dev->wq);
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    return dev;
}

void evdev_describe(struct evdev *dev, const char *phys, int scan)
{
    if (dev) {
        dev->phys = phys;
        dev->scan = scan != 0;
    }
}

void evdev_unregister(struct evdev *dev)
{
    uint32_t flags;

    if (!dev) {
        return;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    dev->gone = 1;
    dev->grab = NULL;
    if (!dev->clients) {
        dev->used = 0;
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    wq_wake_all(&dev->wq);
}

/**
 * Queue `ev` for the open `c`, with the evdev lock held: on a full queue,
 * keep a SYN_DROPPED and `ev` alone. Returns 1 when `ev` ends a packet and
 * the readers are to be woken.
 */
static int pass_event(struct evdev_client *c, const struct input_event *ev)
{
    c->queue[c->head] = *ev;
    c->head = (c->head + 1) & QUEUE_MASK;
    if (c->head == c->tail) {
        c->tail = (c->head - 2) & QUEUE_MASK;
        c->queue[c->tail] = *ev;
        c->queue[c->tail].type = EV_SYN;
        c->queue[c->tail].code = SYN_DROPPED;
        c->queue[c->tail].value = 0;
        c->packet = c->tail;
    }
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        c->packet = c->head;
        return 1;
    }
    return 0;
}

void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    struct evdev_client *c;
    int64_t s, ns;
    uint32_t flags;
    int wake = 0;

    if (!dev) {
        return;
    }
    rtc_realtime(&s, &ns);
    ev.sec = (unsigned long)s;
    ev.usec = (unsigned long)((uint32_t)ns / 1000u); /* ns is below 10^9 */
    ev.type = type;
    ev.code = code;
    ev.value = value;

    flags = spin_lock_irqsave(&evdev_lock);
    if (dev->gone) {
        spin_unlock_irqrestore(&evdev_lock, flags);
        return;
    }
    if (type == EV_KEY) {
        uint8_t bit = (uint8_t)(1u << (code & 7));

        if (code > KEY_MAX || !bit_on(dev->keys, code)) {
            spin_unlock_irqrestore(&evdev_lock, flags);
            return;
        }
        if (value == 0 && !(dev->down[code >> 3] & bit)) {
            spin_unlock_irqrestore(&evdev_lock, flags);
            return;
        }
        if (value == 1 && (dev->down[code >> 3] & bit)) {
            ev.value = 2;
        }
        if (value == 0) {
            dev->down[code >> 3] &= (uint8_t)~bit;
        } else {
            dev->down[code >> 3] |= bit;
        }
    }
    if (dev->grab) {
        wake = pass_event(dev->grab, &ev);
    } else {
        for (c = dev->clients; c; c = c->next) {
            wake |= pass_event(c, &ev);
        }
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (wake) {
        wq_wake(&dev->wq, WQ_DATA);
    }
}

void evdev_report_key(struct evdev *dev, uint32_t scan, uint16_t code, int down)
{
    evdev_report(dev, EV_MSC, MSC_SCAN, (int32_t)scan);
    if (code != 0) {
        evdev_report(dev, EV_KEY, code, down ? 1 : 0);
    }
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

int evdev_present(int n)
{
    if (n < 0 || n >= EVDEV_MAX) {
        return 0;
    }
    return devices[n].used && !devices[n].gone;
}

int32_t evdev_open(fd_entry_t *e, int n)
{
    struct evdev_client *c;
    struct evdev *dev;
    uint32_t flags;

    if (!evdev_present(n)) {
        return -ENODEV;
    }
    c = (struct evdev_client *)kzalloc(sizeof(*c));
    if (!c) {
        return -ENOMEM;
    }
    dev = &devices[n];
    flags = spin_lock_irqsave(&evdev_lock);
    if (dev->gone || !dev->used) {
        spin_unlock_irqrestore(&evdev_lock, flags);
        kfree(c);
        return -ENODEV;
    }
    c->dev = dev;
    c->next = dev->clients;
    dev->clients = c;
    spin_unlock_irqrestore(&evdev_lock, flags);
    e->priv = c;
    return 0;
}

/** The wait condition of a read: a whole packet is queued, or the device
 * has gone. */
static int evdev_readable(void *arg)
{
    struct evdev_client *c = (struct evdev_client *)arg;

    return c->packet != c->tail || c->dev->gone;
}

int32_t evdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct input_event ev[8];
    uint32_t done = 0;

    if (count < sizeof(struct input_event)) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t flags;
        int n = 0;

        flags = spin_lock_irqsave(&evdev_lock);
        if (c->dev->gone) {
            spin_unlock_irqrestore(&evdev_lock, flags);
            return done ? (int32_t)done : -ENODEV;
        }
        while (n < 8 && c->tail != c->packet && done + (n + 1) * sizeof(ev[0]) <= count) {
            ev[n++] = c->queue[c->tail];
            c->tail = (c->tail + 1) & QUEUE_MASK;
        }
        spin_unlock_irqrestore(&evdev_lock, flags);
        if (n > 0) {
            if (copy_to_user(buf + done, ev, n * sizeof(ev[0])) != 0) {
                return -EFAULT;
            }
            done += n * sizeof(ev[0]);
            continue;
        }
        if (done) {
            return (int32_t)done;
        }
        if (e->ofd->status_flags & OFD_NONBLOCK) {
            return -EAGAIN;
        }
        if (wq_wait_event(&c->dev->wq, evdev_readable, c) < 0) {
            return -EINTR;
        }
    }
}

short evdev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;

    *wq = &c->dev->wq;
    if (c->dev->gone) {
        return POLLHUP | POLLERR;
    }
    return (short)(events & (c->packet != c->tail ? POLLIN : 0));
}

/**
 * Copy the string `s` to the user's `arg`, its terminator among it, cut to
 * `len` bytes as Linux cuts it, without a terminator. Returns the bytes
 * copied, -ENOENT for no string, or -EFAULT.
 */
static int32_t string_out(uintptr_t arg, uint32_t len, const char *s)
{
    uint32_t n;

    if (!s) {
        return -ENOENT;
    }
    n = (uint32_t)k_strlen(s) + 1;
    if (n > len) {
        n = len;
    }
    return copy_to_user((void __user *)arg, s, n) != 0 ? -EFAULT : (int32_t)n;
}

/**
 * Copy `bytes` bytes of a bit set to the user's `arg`, cut to `len`, as
 * Linux's `bits_to_user` does; a NULL `bits` gives zeros. Returns the bytes
 * copied, or -EFAULT.
 */
static int32_t bits_out(uintptr_t arg, uint32_t len, const uint8_t *bits, uint32_t bytes)
{
    uint8_t zero[EVDEV_KEY_BYTES];

    if (bytes > len) {
        bytes = len;
    }
    if (!bits) {
        k_memset(zero, 0, sizeof(zero));
        bits = zero;
    }
    return copy_to_user((void __user *)arg, bits, bytes) != 0 ? -EFAULT : (int32_t)bytes;
}

/** EVIOCGRAB: `on` takes the device for the open `c` alone, 0 gives it
 * back. -EBUSY when another open holds it, -EINVAL to give back a grab
 * the open does not hold. */
static int32_t grab(struct evdev_client *c, uintptr_t on)
{
    struct evdev *dev = c->dev;
    int32_t rc = 0;
    uint32_t flags;

    flags = spin_lock_irqsave(&evdev_lock);
    if (on) {
        if (dev->grab) {
            rc = -EBUSY;
        } else {
            dev->grab = c;
        }
    } else if (dev->grab != c) {
        rc = -EINVAL;
    } else {
        dev->grab = NULL;
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    return rc;
}

int32_t evdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct evdev *dev = c->dev;
    uint32_t len = IOC_SIZE(request);
    uint32_t nr = IOC_NR(request);
    uint8_t bits[EVDEV_KEY_BYTES];
    int version = EV_VERSION;
    uint32_t flags;

    if (dev->gone) {
        return -ENODEV;
    }
    if (request == EVIOCGVERSION) {
        return copy_to_user((void __user *)arg, &version, sizeof(version)) != 0 ? -EFAULT : 0;
    }
    if (request == EVIOCGID) {
        return copy_to_user((void __user *)arg, dev->id, sizeof(dev->id)) != 0 ? -EFAULT : 0;
    }
    if (request == EVIOCGRAB) {
        return grab(c, arg);
    }
    if (IOC_TYPE(request) != 'E' || IOC_DIR(request) != IOC_READ) {
        return -ENOTTY;
    }
    switch (nr) {
    case NR_GNAME:
        return string_out(arg, len, dev->name);
    case NR_GPHYS:
        return string_out(arg, len, dev->phys);
    case NR_GUNIQ:
        return string_out(arg, len, NULL);
    case NR_GPROP:
        return bits_out(arg, len, NULL, BITS_BYTES(PROP_CNT));
    case NR_GKEY:
        flags = spin_lock_irqsave(&evdev_lock);
        k_memcpy(bits, dev->down, sizeof(bits));
        spin_unlock_irqrestore(&evdev_lock, flags);
        return bits_out(arg, len, bits, EVDEV_KEY_BYTES);
    case NR_GLED:
        return bits_out(arg, len, NULL, BITS_BYTES(LED_CNT));
    case NR_GSND:
        return bits_out(arg, len, NULL, BITS_BYTES(SND_CNT));
    case NR_GSW:
        return bits_out(arg, len, NULL, BITS_BYTES(SW_CNT));
    case NR_GBIT + 0:
        k_memset(bits, 0, sizeof(bits));
        bits[0] = (uint8_t)(1u << EV_SYN | 1u << EV_KEY | (dev->scan ? 1u << EV_MSC : 0));
        return bits_out(arg, len, bits, BITS_BYTES(EV_CNT));
    case NR_GBIT + EV_KEY:
        return bits_out(arg, len, dev->keys, EVDEV_KEY_BYTES);
    case NR_GBIT + EV_MSC:
        k_memset(bits, 0, sizeof(bits));
        bits[0] = (uint8_t)(dev->scan ? 1u << MSC_SCAN : 0);
        return bits_out(arg, len, bits, BITS_BYTES(MSC_CNT));
    /* The types a device here never has, each with its own count. */
    case NR_GBIT + EV_REL:
        return bits_out(arg, len, NULL, BITS_BYTES(REL_CNT));
    case NR_GBIT + EV_ABS:
        return bits_out(arg, len, NULL, BITS_BYTES(ABS_CNT));
    case NR_GBIT + EV_SW:
        return bits_out(arg, len, NULL, BITS_BYTES(SW_CNT));
    case NR_GBIT + EV_LED:
        return bits_out(arg, len, NULL, BITS_BYTES(LED_CNT));
    case NR_GBIT + EV_SND:
        return bits_out(arg, len, NULL, BITS_BYTES(SND_CNT));
    case NR_GBIT + EV_FF:
        return bits_out(arg, len, NULL, BITS_BYTES(FF_CNT));
    default:
        break;
    }
    if (nr > NR_GBIT && nr < NR_GBIT + EV_CNT) {
        return -EINVAL;
    }
    return -ENOTTY;
}

void evdev_close(fd_entry_t *e)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct evdev_client **p;
    struct evdev *dev;
    uint32_t flags;

    if (!c) {
        return;
    }
    dev = c->dev;
    flags = spin_lock_irqsave(&evdev_lock);
    for (p = &dev->clients; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    if (dev->grab == c) {
        dev->grab = NULL;
    }
    if (dev->gone && !dev->clients) {
        dev->used = 0;
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    e->priv = NULL;
    kfree(c);
}
