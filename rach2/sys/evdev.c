/*
 * evdev.c: the input devices of /dev/input/eventN (evdev.h), with Linux's
 * evdev records, queues and ioctls.
 */

#include "evdev.h"

#include "input.h"
#include "io/poll.h"
#include "lib/kdef.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "mm/kheap.h"
#include "mm/usercopy.h"
#include "mouse.h"
#include "proc/wait_queue.h"
#include "rtc.h"
#include "syscall.h"

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

/* A record as an i386 program reads it from the x86-64 kernel, Linux's
 * `input_event_compat`: 32-bit seconds and microseconds, 16 bytes. */
struct input_event32 {
    uint32_t sec;
    uint32_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* The most bytes a write takes, as Linux's evdev_write: 170 records of 24
 * bytes. */
#define WRITE_MAX 4096u

/* The fewest records an open queues, and the packets its queue holds, as
 * Linux's EVDEV_MIN_BUFFER_SIZE and EVDEV_BUF_PACKETS (`queue_size`). */
#define EVDEV_QUEUE   64
#define EVDEV_PACKETS 8

/* The largest tracking number, after which they start again at 0, and its
 * top bit, by which the older of two numbers is told across the wrap. */
#define TRKID_MAX 0xffff
#define TRKID_SGN 0x8000

/* The values a slot keeps, one for each multitouch axis. */
#define MT_VALUES (ABS_MT_LAST - ABS_MT_FIRST + 1)

/* The ioctl numbers are Linux's: _IOC(dir, 'E', nr, size), with the
 * direction in the top two bits, the size in the next fourteen, then the
 * type and the number. */
#define IOC_WRITE     1u
#define IOC_READ      2u
#define IOC_DIR(r)    ((r) >> 30)
#define IOC_SIZE(r)   (((r) >> 16) & 0x3FFFu)
#define IOC_TYPE(r)   (((r) >> 8) & 0xFFu)
#define IOC_NR(r)     ((r) & 0xFFu)
#define EVIOCGVERSION 0x80044501u
#define EVIOCGID      0x80084502u
#define EVIOCGREP     0x80084503u /* _IOR('E', 0x03, unsigned int[2]) */
#define EVIOCSREP     0x40084503u /* _IOW('E', 0x03, unsigned int[2]) */
#define EVIOCGRAB     0x40044590u
#define EVIOCREVOKE   0x40044591u
#define EVIOCGMASK    0x80104592u /* _IOR('E', 0x92, struct input_mask) */
#define EVIOCSMASK    0x40104593u /* _IOW('E', 0x93, struct input_mask) */
#define EVIOCSCLOCKID 0x400445A0u
#define NR_GNAME      0x06u
#define NR_GPHYS      0x07u
#define NR_GUNIQ      0x08u
#define NR_GPROP      0x09u
#define NR_GMTSLOTS   0x0Au /* EVIOCGMTSLOTS(len): a code, then a value a slot */
#define NR_GKEY       0x18u
#define NR_GLED       0x19u
#define NR_GSND       0x1Au
#define NR_GSW        0x1Bu
#define NR_GBIT       0x20u /* to 0x3F: EVIOCGBIT(type) is 0x20 + type */
#define NR_GABS       0x40u /* to 0x7F: EVIOCGABS(axis) is 0x40 + axis */
#define NR_SABS       0xC0u /* to 0xFF: EVIOCSABS(axis), a write */
#define EV_VERSION    0x010001
#define BITS_LONGS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define BITS_BYTES(n) (BITS_LONGS(n) * sizeof(long))

/* The clocks of EVIOCSCLOCKID, clock_gettime's numbers, and the index of
 * each in a packet's stamps. */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7
#define CLK_REAL        0
#define CLK_MONO        1
#define CLK_BOOT        2
#define CLKS            3

/* EVIOCGMASK's and EVIOCSMASK's argument, the same 16 bytes on both ABIs:
 * the type, and the user's buffer of a bit for each code of it. */
struct input_mask {
    uint32_t type;
    uint32_t codes_size; /* the buffer's bytes */
    uint64_t codes_ptr;
};

/*
 * An open of a device, a client in Linux's word. The records from `tail` to
 * `packet` are whole packets a read may take; those from `packet` to `head`
 * are the packet still coming. A full queue keeps only a SYN_DROPPED and
 * the newest record, as Linux's does, so that a reader that fell behind
 * learns that it lost records and starts again at a packet's end.
 *
 * The masks are EVIOCSMASK's, one set of bits for each type that has one,
 * `mask_ev` that of the types themselves; a type with no mask set passes
 * every code.
 */
struct evdev_client {
    struct evdev *dev;
    struct evdev_client *next; /* the device's other opens */
    unsigned head;
    unsigned tail;
    unsigned packet;
    unsigned qmask;  /* the queue's size less one; the size is a power of two */
    int clock;       /* CLK_*, the clock its records are stamped by */
    int revoked;     /* EVIOCREVOKE: it gets nothing, and reads -ENODEV */
    uint32_t masked; /* a bit 1 << type for each type with a mask set */
    uint8_t mask_ev[BITS_BYTES(EV_CNT)];
    uint8_t mask_key[BITS_BYTES(KEY_CNT)];
    uint8_t mask_rel[BITS_BYTES(REL_CNT)];
    uint8_t mask_abs[BITS_BYTES(ABS_CNT)];
    uint8_t mask_msc[BITS_BYTES(MSC_CNT)];
    uint8_t mask_sw[BITS_BYTES(SW_CNT)];
    uint8_t mask_led[BITS_BYTES(LED_CNT)];
    uint8_t mask_snd[BITS_BYTES(SND_CNT)];
    uint8_t mask_ff[BITS_BYTES(FF_CNT)];
    struct input_event *queue; /* qmask + 1 records, from the heap */
};

/* A slot of a device that keeps slots: the value of each multitouch axis,
 * ABS_MT_TRACKING_ID -1 while it has no contact, and the frame that last
 * named it (`evdev_mt_report_slot_state`). */
struct evdev_slot {
    int32_t abs[MT_VALUES];
    unsigned int frame;
};

struct evdev {
    int used;           /* the slot is taken, by a device or by the opens of one gone */
    int gone;           /* unregistered: no node, and its opens read -ENODEV */
    unsigned int flags; /* EVDEV_SCAN, EVDEV_KEYBOARD */
    unsigned int leds;  /* a keyboard's LEDs lit, a bit 1 << LED_* each */
    int rep[2];         /* a keyboard's REP_DELAY and REP_PERIOD, ms */
    int has_abs;        /* EV_ABS: `absinfo` answers EVIOCGABS */
    int stamped;        /* a packet has begun, at `stamp` */
    uint32_t gen;       /* the registration's number, `evdev_desc`'s */
    const char *name;
    const char *phys;                          /* NULL: EVIOCGPHYS answers -ENOENT */
    uint16_t id[4];                            /* bustype, vendor, product, version */
    uint8_t keys[EVDEV_KEY_BYTES];             /* the keys it can report */
    uint8_t down[EVDEV_KEY_BYTES];             /* the keys down now, for EVIOCGKEY */
    uint8_t rel[BITS_BYTES(REL_CNT)];          /* the relative axes it has */
    uint8_t abs[BITS_BYTES(ABS_CNT)];          /* the absolute axes it has */
    uint8_t props[BITS_BYTES(INPUT_PROP_CNT)]; /* INPUT_PROP_* */
    struct input_absinfo absinfo[ABS_CNT];
    int num_slots;         /* the slots it keeps, 0 for none */
    int slot;              /* the slot the last ABS_MT_SLOT chose */
    unsigned int mt_flags; /* INPUT_MT_* */
    unsigned int frame;    /* the slots' frame, ended by `evdev_mt_drop_unused` */
    unsigned int trkid;    /* the next tracking number, before the cut to TRKID_MAX */
    struct evdev_slot slots[EVDEV_MT_SLOTS];
    evdev_handler_t handler;
    int64_t stamp_sec[CLKS]; /* the packet's time on each clock */
    int32_t stamp_nsec[CLKS];
    struct evdev_client *clients;
    struct evdev_client *grab; /* the open that holds EVIOCGRAB, or NULL */
    wait_queue_t wq;           /* its readers and pollers */
};

static struct evdev devices[EVDEV_MAX];

/* Over the devices and their opens' queues. */
static struct spinlock evdev_lock = SPINLOCK_INIT;

/* The registrations so far, which number each, and the registrations and
 * unregistrations, whose waiters `change_wq` holds; static storage starts
 * it empty, so a change before anyone waits can wake it. */
static uint32_t registered_count;
static uint32_t change_count;
static wait_queue_t change_wq;

/** Whether bit `n` of the set `bits` is on. */
static int bit_on(const uint8_t *bits, unsigned n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

/** Turn bit `n` of the set `bits` on. */
static void set_bit(uint8_t *bits, unsigned n)
{
    bits[n >> 3] |= (uint8_t)(1u << (n & 7));
}

/**
 * Read the clocks a record may be stamped by, now, into `sec` and `nsec`,
 * indexed by CLK_*: the real time and the monotonic clock as clock_gettime
 * reads them, and boot time, which clock_gettime reads as the monotonic
 * clock (syscall.c's `clock_read`). Any context.
 */
static void read_clocks(int64_t *sec, int32_t *nsec)
{
    int64_t s, ns;

    rtc_realtime(&s, &ns);
    sec[CLK_REAL] = s;
    nsec[CLK_REAL] = (int32_t)ns;
    rtc_monotonic(&s, &ns);
    sec[CLK_MONO] = sec[CLK_BOOT] = s;
    nsec[CLK_MONO] = nsec[CLK_BOOT] = (int32_t)ns;
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
        dev->gen = ++registered_count;
        change_count++;
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (dev) {
        wq_wake_all(&change_wq);
    }
    return dev;
}

void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags)
{
    if (dev) {
        dev->phys = phys;
        dev->flags = flags;
    }
}

void evdev_set_version(struct evdev *dev, uint16_t version)
{
    if (dev) {
        dev->id[3] = version;
    }
}

void evdev_set_capability(struct evdev *dev, uint16_t type, uint16_t code)
{
    if (!dev) {
        return;
    }
    if (type == EV_KEY && code <= KEY_MAX) {
        set_bit(dev->keys, code);
    } else if (type == EV_REL && code < REL_CNT) {
        set_bit(dev->rel, code);
    }
}

void evdev_set_abs(struct evdev *dev, uint16_t code, int32_t minimum, int32_t maximum, int32_t fuzz,
                   int32_t flat)
{
    struct input_absinfo *a;

    if (!dev || code >= ABS_CNT) {
        return;
    }
    a = &dev->absinfo[code];
    a->minimum = minimum;
    a->maximum = maximum;
    a->fuzz = fuzz;
    a->flat = flat;
    set_bit(dev->abs, code);
    dev->has_abs = 1;
}

void evdev_set_abs_res(struct evdev *dev, uint16_t code, int32_t resolution)
{
    if (dev && code < ABS_CNT) {
        dev->absinfo[code].resolution = resolution;
    }
}

void evdev_set_prop(struct evdev *dev, unsigned int prop)
{
    if (dev && prop < INPUT_PROP_CNT) {
        set_bit(dev->props, prop);
    }
}

/** Copy the absolute axis `src` of `dev` to `dst` without its fuzz, as
 * Linux's `copy_abs`, when the device has `src`. */
static void copy_abs(struct evdev *dev, uint16_t dst, uint16_t src)
{
    if (bit_on(dev->abs, src)) {
        dev->absinfo[dst] = dev->absinfo[src];
        dev->absinfo[dst].fuzz = 0;
        set_bit(dev->abs, dst);
    }
}

int evdev_init_slots(struct evdev *dev, int num_slots, unsigned int flags)
{
    int i;

    if (!dev || num_slots < 1 || num_slots > EVDEV_MT_SLOTS) {
        return -EINVAL;
    }
    if (dev->num_slots) {
        return dev->num_slots != num_slots ? -EINVAL : 0;
    }
    dev->num_slots = num_slots;
    dev->mt_flags = flags;
    evdev_set_abs(dev, ABS_MT_SLOT, 0, num_slots - 1, 0, 0);
    evdev_set_abs(dev, ABS_MT_TRACKING_ID, 0, TRKID_MAX, 0, 0);
    if (flags & (INPUT_MT_POINTER | INPUT_MT_DIRECT)) {
        set_bit(dev->keys, BTN_TOUCH);
        copy_abs(dev, ABS_X, ABS_MT_POSITION_X);
        copy_abs(dev, ABS_Y, ABS_MT_POSITION_Y);
        copy_abs(dev, ABS_PRESSURE, ABS_MT_PRESSURE);
    }
    if (flags & INPUT_MT_POINTER) {
        set_bit(dev->keys, BTN_TOOL_FINGER);
        set_bit(dev->keys, BTN_TOOL_DOUBLETAP);
        if (num_slots >= 3) {
            set_bit(dev->keys, BTN_TOOL_TRIPLETAP);
        }
        if (num_slots >= 4) {
            set_bit(dev->keys, BTN_TOOL_QUADTAP);
        }
        if (num_slots >= 5) {
            set_bit(dev->keys, BTN_TOOL_QUINTTAP);
        }
        set_bit(dev->props, INPUT_PROP_POINTER);
    }
    if (flags & INPUT_MT_DIRECT) {
        set_bit(dev->props, INPUT_PROP_DIRECT);
    }
    if (flags & INPUT_MT_SEMI_MT) {
        set_bit(dev->props, INPUT_PROP_SEMI_MT);
    }
    for (i = 0; i < num_slots; i++) {
        dev->slots[i].abs[ABS_MT_TRACKING_ID - ABS_MT_FIRST] = -1;
    }
    dev->frame = 1; /* no slot is used in the first frame */
    return 0;
}

int evdev_mt_report_slot_state(struct evdev *dev, int tool, int active)
{
    struct evdev_slot *slot;
    uint32_t flags;
    int32_t id;

    if (!dev || !dev->num_slots) {
        return 0;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    slot = &dev->slots[dev->slot];
    slot->frame = dev->frame;
    id = slot->abs[ABS_MT_TRACKING_ID - ABS_MT_FIRST];
    if (active && id < 0) {
        id = (int32_t)(dev->trkid++ & TRKID_MAX);
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (!active) {
        evdev_report(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
        return 0;
    }
    evdev_report(dev, EV_ABS, ABS_MT_TRACKING_ID, id);
    evdev_report(dev, EV_ABS, ABS_MT_TOOL_TYPE, tool);
    return 1;
}

void evdev_mt_report_finger_count(struct evdev *dev, int count)
{
    evdev_report(dev, EV_KEY, BTN_TOOL_FINGER, count == 1);
    evdev_report(dev, EV_KEY, BTN_TOOL_DOUBLETAP, count == 2);
    evdev_report(dev, EV_KEY, BTN_TOOL_TRIPLETAP, count == 3);
    evdev_report(dev, EV_KEY, BTN_TOOL_QUADTAP, count == 4);
    evdev_report(dev, EV_KEY, BTN_TOOL_QUINTTAP, count == 5);
}

void evdev_mt_report_pointer_emulation(struct evdev *dev, int use_count)
{
    struct evdev_slot *oldest = NULL;
    int32_t x = 0, y = 0, p, reported_p = 0;
    int32_t oldid, id;
    int has_pressure;
    uint32_t flags;
    int count = 0;
    int i;

    if (!dev || !dev->num_slots) {
        return;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    has_pressure = bit_on(dev->abs, ABS_MT_PRESSURE);
    oldid = (int32_t)dev->trkid;
    for (i = 0; i < dev->num_slots; i++) {
        struct evdev_slot *s = &dev->slots[i];

        id = s->abs[ABS_MT_TRACKING_ID - ABS_MT_FIRST];
        if (id < 0) {
            continue;
        }
        if ((id - oldid) & TRKID_SGN) {
            oldest = s;
            oldid = id;
        }
        if (has_pressure) {
            p = s->abs[ABS_MT_PRESSURE - ABS_MT_FIRST];
            if (oldid == id) {
                reported_p = p;
            }
        }
        count++;
    }
    if (oldest) {
        x = oldest->abs[ABS_MT_POSITION_X - ABS_MT_FIRST];
        y = oldest->abs[ABS_MT_POSITION_Y - ABS_MT_FIRST];
    }
    spin_unlock_irqrestore(&evdev_lock, flags);

    evdev_report(dev, EV_KEY, BTN_TOUCH, count > 0);
    if (use_count) {
        evdev_mt_report_finger_count(dev, count);
    }
    if (oldest) {
        evdev_report(dev, EV_ABS, ABS_X, x);
        evdev_report(dev, EV_ABS, ABS_Y, y);
        if (has_pressure) {
            evdev_report(dev, EV_ABS, ABS_PRESSURE, reported_p);
        }
    } else if (has_pressure) {
        evdev_report(dev, EV_ABS, ABS_PRESSURE, 0);
    }
}

void evdev_mt_drop_unused(struct evdev *dev)
{
    int lift[EVDEV_MT_SLOTS];
    uint32_t flags;
    int i, n = 0;

    if (!dev || !dev->num_slots) {
        return;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    for (i = 0; i < dev->num_slots; i++) {
        if (dev->slots[i].abs[ABS_MT_TRACKING_ID - ABS_MT_FIRST] >= 0 &&
            dev->slots[i].frame != dev->frame) {
            lift[n++] = i;
        }
    }
    dev->frame++;
    spin_unlock_irqrestore(&evdev_lock, flags);
    for (i = 0; i < n; i++) {
        evdev_report(dev, EV_ABS, ABS_MT_SLOT, lift[i]);
        evdev_report(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
}

/**
 * One step of Linux's `adjust_dual`, over the `step`-strided entries of a
 * row or column of the cost matrix from `begin` to `end`: subtract from
 * each the mean of its two least, so that one entry goes negative. `eq` and
 * `mu` are as Linux passes them. Returns whether the step changed which
 * entries are negative.
 */
static int adjust_dual(int *begin, int step, int *end, int eq, int mu)
{
    int f, *p, s, c;

    if (begin == end) {
        return 0;
    }
    f = *begin;
    p = begin + step;
    s = p == end ? f + 1 : *p;
    for (; p != end; p += step) {
        if (*p < f) {
            s = f;
            f = *p;
        } else if (*p < s) {
            s = *p;
        }
    }
    c = (f + s + 1) / 2;
    if (c == 0 || (c > mu && (!eq || mu > 0))) {
        return 0;
    }
    if (s < 0 && mu <= 0) {
        c *= 2; /* Linux's penalty of overcovers, for positive matrices */
    }
    for (p = begin; p != end; p += step) {
        *p -= c;
    }
    return (c < s && s <= 0) || (f >= 0 && f < c);
}

/** Linux's `find_reduced_matrix`: reduce the `nrc` costs at `w`, `nr`
 * contacts to a row of each of the `nc` slots with contacts, until each
 * contact has at most one negative entry. */
static void find_reduced_matrix(int *w, int nr, int nc, int nrc, int mu)
{
    int i, k, sum;

    for (k = 0; k < nrc; k++) {
        for (i = 0; i < nr; i++) {
            adjust_dual(w + i, nr, w + i + nrc, nr <= nc, mu);
        }
        sum = 0;
        for (i = 0; i < nrc; i += nr) {
            sum += adjust_dual(w + i, 1, w + i + nr, nc <= nr, mu);
        }
        if (!sum) {
            break;
        }
    }
}

int evdev_mt_assign_slots(struct evdev *dev, int *slots, const struct evdev_mt_pos *pos,
                          int num_pos, int dmax)
{
    int w[EVDEV_MT_SLOTS * EVDEV_MT_SLOTS];
    int active[EVDEV_MT_SLOTS];
    int mu = 2 * dmax * dmax;
    uint32_t flags;
    int i, j, nrc = 0, *row;

    if (!dev || !dev->num_slots || !(dev->mt_flags & INPUT_MT_TRACK)) {
        return -ENXIO;
    }
    if (num_pos > dev->num_slots) {
        return -EINVAL;
    }
    if (num_pos < 1) {
        return 0;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    for (i = 0; i < dev->num_slots; i++) {
        const int32_t *v = dev->slots[i].abs;

        active[i] = v[ABS_MT_TRACKING_ID - ABS_MT_FIRST] >= 0;
        if (!active[i]) {
            continue;
        }
        for (j = 0; j < num_pos; j++) {
            int dx = v[ABS_MT_POSITION_X - ABS_MT_FIRST] - pos[j].x;
            int dy = v[ABS_MT_POSITION_Y - ABS_MT_FIRST] - pos[j].y;

            w[nrc++] = dx * dx + dy * dy - mu;
        }
    }
    spin_unlock_irqrestore(&evdev_lock, flags);

    find_reduced_matrix(w, num_pos, nrc / num_pos, nrc, mu);
    for (j = 0; j < num_pos; j++) {
        slots[j] = -1;
    }
    row = w;
    for (i = 0; i < dev->num_slots; i++) {
        if (!active[i]) {
            continue;
        }
        for (j = 0; j < num_pos; j++) {
            if (row[j] < 0) {
                slots[j] = i;
                break;
            }
        }
        row += num_pos;
    }
    for (i = 0; i < dev->num_slots; i++) {
        if (active[i]) {
            continue;
        }
        for (j = 0; j < num_pos; j++) {
            if (slots[j] < 0) {
                slots[j] = i;
                break;
            }
        }
    }
    return 0;
}

int evdev_get_abs(struct evdev *dev, uint16_t code, struct input_absinfo *out)
{
    uint32_t flags;

    if (!dev || code >= ABS_CNT || !bit_on(dev->abs, code)) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    *out = dev->absinfo[code];
    spin_unlock_irqrestore(&evdev_lock, flags);
    return 0;
}

void evdev_set_handler(struct evdev *dev, evdev_handler_t fn)
{
    if (dev) {
        dev->handler = fn;
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
    change_count++;
    spin_unlock_irqrestore(&evdev_lock, flags);
    wq_wake_all(&dev->wq);
    wq_wake_all(&change_wq);
}

/**
 * The mask of EVIOCSMASK the open `c` keeps for `type`, with in `*count` the
 * codes it covers: EV_CNT, the types themselves, for EV_SYN, as EVIOCGBIT
 * has it. NULL with a count of 0 for a type no mask is kept for.
 */
static uint8_t *client_mask(struct evdev_client *c, uint32_t type, unsigned *count)
{
    switch (type) {
    case EV_SYN:
        *count = EV_CNT;
        return c->mask_ev;
    case EV_KEY:
        *count = KEY_CNT;
        return c->mask_key;
    case EV_REL:
        *count = REL_CNT;
        return c->mask_rel;
    case EV_ABS:
        *count = ABS_CNT;
        return c->mask_abs;
    case EV_MSC:
        *count = MSC_CNT;
        return c->mask_msc;
    case EV_SW:
        *count = SW_CNT;
        return c->mask_sw;
    case EV_LED:
        *count = LED_CNT;
        return c->mask_led;
    case EV_SND:
        *count = SND_CNT;
        return c->mask_snd;
    case EV_FF:
        *count = FF_CNT;
        return c->mask_ff;
    default:
        *count = 0;
        return NULL;
    }
}

/** Whether the masks of `c` leave out the event `type`, `code`: never an
 * EV_SYN, nor a type or code no mask covers. */
static int filtered(struct evdev_client *c, unsigned type, unsigned code)
{
    uint8_t *mask;
    unsigned count;

    if (type == EV_SYN || type >= EV_CNT) {
        return 0;
    }
    if ((c->masked & 1u) && !bit_on(c->mask_ev, type)) {
        return 1;
    }
    mask = client_mask(c, type, &count);
    if (!mask || code >= count) {
        return 0;
    }
    return (c->masked & (1u << type)) && !bit_on(mask, code);
}

/**
 * Queue the event `type`, `code`, `value` of `dev` for the open `c`, with
 * the evdev lock held, stamped with the packet's time on the open's clock:
 * nothing for a revoked open, an event its masks leave out, or a SYN_REPORT
 * that would end an empty packet; on a full queue, keep a SYN_DROPPED and
 * the event alone. Returns 1 when the event ends a packet and the readers
 * are to be woken.
 */
static int pass_event(struct evdev_client *c, const struct evdev *dev, uint16_t type, uint16_t code,
                      int32_t value)
{
    struct input_event ev;
    int report = type == EV_SYN && code == SYN_REPORT;

    if (c->revoked || filtered(c, type, code) || (report && c->packet == c->head)) {
        return 0;
    }
    ev.sec = (unsigned long)dev->stamp_sec[c->clock];
    ev.usec = (unsigned long)((uint32_t)dev->stamp_nsec[c->clock] / 1000u); /* below 10^9 */
    ev.type = type;
    ev.code = code;
    ev.value = value;
    c->queue[c->head] = ev;
    c->head = (c->head + 1) & c->qmask;
    if (c->head == c->tail) {
        c->tail = (c->head - 2) & c->qmask;
        c->queue[c->tail] = ev;
        c->queue[c->tail].type = EV_SYN;
        c->queue[c->tail].code = SYN_DROPPED;
        c->queue[c->tail].value = 0;
        c->packet = c->tail;
    }
    if (report) {
        c->packet = c->head;
        return 1;
    }
    return 0;
}

/** `a` + `b` in 32 bits, wrapping as Linux's kernel, built with
 * -fno-strict-overflow, computes it; a user's EVIOCSABS sets any fuzz. */
static int32_t wrap_add(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

/** Linux's `input_defuzz_abs_event`: `value` drawn toward `old` the more,
 * the nearer it is to it within `fuzz`, and `old` itself within half, in
 * Linux's wrapping arithmetic. */
static int32_t defuzz(int32_t value, int32_t old, int32_t fuzz)
{
    int32_t twice = (int32_t)((uint32_t)fuzz * 2u);

    if (fuzz) {
        if (value > wrap_add(old, -(fuzz / 2)) && value < wrap_add(old, fuzz / 2)) {
            return old;
        }
        if (value > (int32_t)((uint32_t)old - (uint32_t)fuzz) && value < wrap_add(old, fuzz)) {
            return wrap_add((int32_t)((uint32_t)old * 3u), value) / 4;
        }
        if (value > (int32_t)((uint32_t)old - (uint32_t)twice) && value < wrap_add(old, twice)) {
            return wrap_add(old, value) / 2;
        }
    }
    return value;
}

/* What `accept_event` decides of an event. */
#define DROP      0 /* it goes to no open */
#define PASS      1 /* it goes to the opens */
#define PASS_SLOT 2 /* it goes after an ABS_MT_SLOT of the slot it is for */

/**
 * The absolute axes' part of `accept_event`, Linux's
 * `input_handle_abs_event`: ABS_MT_SLOT only chooses the slot of the
 * multitouch values after it, and is itself held back until one of them
 * passes; a multitouch value is filtered against its slot's value on a
 * device that keeps slots and passes unfiltered on one that does not;
 * another value is filtered against the axis's.
 */
static int accept_abs(struct evdev *dev, uint16_t code, int32_t *value)
{
    int32_t *old;
    int new_slot = 0;

    if (code == ABS_MT_SLOT) {
        if (dev->num_slots && *value >= 0 && *value < dev->num_slots) {
            dev->slot = *value;
        }
        return DROP;
    }
    if (code < ABS_MT_FIRST || code > ABS_MT_LAST) {
        old = &dev->absinfo[code].value;
    } else if (dev->num_slots) {
        old = &dev->slots[dev->slot].abs[code - ABS_MT_FIRST];
        new_slot = dev->slot != dev->absinfo[ABS_MT_SLOT].value;
    } else {
        return PASS;
    }
    *value = defuzz(*value, *old, dev->absinfo[code].fuzz);
    if (*value == *old) {
        return DROP;
    }
    *old = *value;
    if (new_slot) {
        dev->absinfo[ABS_MT_SLOT].value = dev->slot;
        return PASS_SLOT;
    }
    return PASS;
}

/**
 * The input core's filter, with the evdev lock held: whether the event
 * `type`, `code`, `*value` of `dev` goes to its opens, DROP, PASS or
 * PASS_SLOT, as Linux's `input_get_disposition` decides, keeping the keys
 * down, each absolute axis's value and each slot's. An absolute value is
 * defuzzed in `*value`.
 */
static int accept_event(struct evdev *dev, uint16_t type, uint16_t code, int32_t *value)
{
    uint8_t bit;

    switch (type) {
    case EV_KEY:
        if (code > KEY_MAX || !bit_on(dev->keys, code)) {
            return 0;
        }
        if (*value == 2) {
            return 1;
        }
        bit = (uint8_t)(1u << (code & 7));
        if (((dev->down[code >> 3] & bit) != 0) == (*value != 0)) {
            return 0;
        }
        dev->down[code >> 3] ^= bit;
        return 1;
    case EV_REL:
        return code < REL_CNT && bit_on(dev->rel, code) && *value != 0;
    case EV_ABS:
        if (code >= ABS_CNT || !bit_on(dev->abs, code)) {
            return 0;
        }
        return accept_abs(dev, code, value);
    case EV_MSC:
        return code == MSC_SCAN && (dev->flags & EVDEV_SCAN);
    case EV_SYN:
        return code == SYN_REPORT || code == SYN_CONFIG || code == SYN_MT_REPORT;
    default:
        return 1;
    }
}

/** Queue the event `type`, `code`, `value` for the opens of `dev`, or for
 * the open that holds the grab, with the evdev lock held. Returns whether
 * the readers are to be woken. */
static int pass_all(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    struct evdev_client *c;
    int wake = 0;

    if (dev->grab) {
        return pass_event(dev->grab, dev, type, code, value);
    }
    for (c = dev->clients; c; c = c->next) {
        wake |= pass_event(c, dev, type, code, value);
    }
    return wake;
}

/**
 * `evdev_report`, which also says whether the event passed the filters and
 * went out, 1, or was dropped, 0, and in `*grabbed` whether an open held the
 * grab when it went out.
 */
static int report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value, int *grabbed)
{
    evdev_handler_t handler;
    int64_t sec[CLKS];
    int32_t nsec[CLKS];
    uint32_t flags;
    int32_t slot = 0;
    int wake = 0;
    int how;
    int i;

    *grabbed = 0;
    if (!dev) {
        return 0;
    }
    read_clocks(sec, nsec);
    flags = spin_lock_irqsave(&evdev_lock);
    how = dev->gone ? DROP : accept_event(dev, type, code, &value);
    if (how == DROP) {
        spin_unlock_irqrestore(&evdev_lock, flags);
        return 0;
    }
    if (!dev->stamped) {
        for (i = 0; i < CLKS; i++) {
            dev->stamp_sec[i] = sec[i];
            dev->stamp_nsec[i] = nsec[i];
        }
        dev->stamped = 1;
    }
    if (how == PASS_SLOT) {
        slot = dev->slot;
        wake |= pass_all(dev, EV_ABS, ABS_MT_SLOT, slot);
    }
    wake |= pass_all(dev, type, code, value);
    if (type == EV_SYN && code == SYN_REPORT) {
        dev->stamped = 0;
    }
    *grabbed = dev->grab != NULL;
    handler = dev->grab ? NULL : dev->handler;
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (wake) {
        wq_wake(&dev->wq, WQ_DATA);
    }
    if (handler) {
        if (how == PASS_SLOT) {
            handler(dev, EV_ABS, ABS_MT_SLOT, slot);
        }
        handler(dev, type, code, value);
    }
    return 1;
}

void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    int grabbed;

    report(dev, type, code, value, &grabbed);
}

int evdev_grabbed(struct evdev *dev)
{
    uint32_t flags;
    int grabbed;

    if (!dev) {
        return 0;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    grabbed = dev->grab != NULL;
    spin_unlock_irqrestore(&evdev_lock, flags);
    return grabbed;
}

void evdev_report_key(struct evdev *dev, uint32_t scan, uint16_t code, int down)
{
    evdev_report(dev, EV_MSC, MSC_SCAN, (int32_t)scan);
    if (code != 0) {
        evdev_report(dev, EV_KEY, code, down ? 1 : 0);
    }
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/**
 * Report, for the keyboard `dev`, an event of `type` for each code from 0 to
 * `n` - 1 whose value in `now` differs from `old[code]`, setting `old` to
 * `now`, then SYN_REPORT when any did.
 */
static void report_changes(struct evdev *dev, uint16_t type, int *old, const int *now, int n)
{
    int changed[4];
    uint32_t flags;
    int i, any = 0;

    flags = spin_lock_irqsave(&evdev_lock);
    for (i = 0; i < n; i++) {
        changed[i] = old[i] != now[i];
        any |= changed[i];
        old[i] = now[i];
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    for (i = 0; i < n; i++) {
        if (changed[i]) {
            evdev_report(dev, type, (uint16_t)i, now[i]);
        }
    }
    if (any) {
        evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    }
}

void evdev_report_leds(struct evdev *dev, unsigned int mask)
{
    int old[3], now[3], i;

    if (!dev || !(dev->flags & EVDEV_KEYBOARD)) {
        return;
    }
    for (i = 0; i < 3; i++) {
        old[i] = (int)(dev->leds >> i) & 1;
        now[i] = (int)(mask >> i) & 1;
    }
    report_changes(dev, EV_LED, old, now, 3);
    dev->leds = mask & 7u;
}

void evdev_report_repeat(struct evdev *dev, int delay_ms, int period_ms)
{
    int now[2];

    if (!dev || !(dev->flags & EVDEV_KEYBOARD)) {
        return;
    }
    now[REP_DELAY] = delay_ms;
    now[REP_PERIOD] = period_ms;
    report_changes(dev, EV_REP, dev->rep, now, 2);
}

int evdev_present(int n)
{
    if (n < 0 || n >= EVDEV_MAX) {
        return 0;
    }
    return devices[n].used && !devices[n].gone;
}

/** The number of bits set in the `n` bytes at `bits`. */
static unsigned bits_set(const uint8_t *bits, unsigned n)
{
    unsigned i, count = 0;
    uint8_t b;

    for (i = 0; i < n; i++) {
        for (b = bits[i]; b; b &= (uint8_t)(b - 1)) {
            count++;
        }
    }
    return count;
}

/**
 * The records an open of `dev` queues, as Linux's evdev sizes its buffer:
 * room for EVDEV_PACKETS packets of the size that Linux's
 * `input_estimate_events_per_packet` guesses from the axes and slots, and
 * at least EVDEV_QUEUE, rounded up to a power of two. A keyboard's queue
 * holds 64 records, a mouse's 128 and a touchpad's with two slots 256.
 */
static unsigned queue_size(const struct evdev *dev)
{
    unsigned events, size;
    int mt_slots = 0;
    unsigned i;

    if (dev->num_slots) {
        mt_slots = dev->num_slots;
    } else if (bit_on(dev->abs, ABS_MT_TRACKING_ID)) {
        mt_slots =
            dev->absinfo[ABS_MT_TRACKING_ID].maximum - dev->absinfo[ABS_MT_TRACKING_ID].minimum + 1;
        mt_slots = mt_slots < 2 ? 2 : mt_slots > 32 ? 32 : mt_slots;
    } else if (bit_on(dev->abs, ABS_MT_POSITION_X)) {
        mt_slots = 2;
    }
    events = (unsigned)mt_slots + 1; /* SYN_MT_REPORT and SYN_REPORT */
    for (i = 0; i < ABS_CNT; i++) {
        if (bit_on(dev->abs, i)) {
            events += (i == ABS_MT_SLOT || (i >= ABS_MT_FIRST && i <= ABS_MT_LAST))
                          ? (unsigned)mt_slots
                          : 1u;
        }
    }
    events += bits_set(dev->rel, sizeof(dev->rel));
    events += 7; /* the keys and MSC codes */
    events *= EVDEV_PACKETS;
    size = EVDEV_QUEUE;
    while (size < events) {
        size *= 2;
    }
    return size;
}

int32_t evdev_open(fd_entry_t *e, int n)
{
    struct evdev_client *c;
    struct evdev *dev;
    uint32_t flags;
    unsigned size;

    if (!evdev_present(n)) {
        return -ENODEV;
    }
    dev = &devices[n];
    size = queue_size(dev);
    c = (struct evdev_client *)kzalloc(sizeof(*c));
    if (!c) {
        return -ENOMEM;
    }
    c->queue = (struct input_event *)kzalloc(size * sizeof(struct input_event));
    if (!c->queue) {
        kfree(c);
        return -ENOMEM;
    }
    c->qmask = size - 1;
    c->clock = CLK_REAL;
    flags = spin_lock_irqsave(&evdev_lock);
    if (dev->gone || !dev->used) {
        spin_unlock_irqrestore(&evdev_lock, flags);
        kfree(c->queue);
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

/** Whether the open `c` reads -ENODEV: its device has gone, or it was
 * revoked. */
static int dead(const struct evdev_client *c)
{
    return c->dev->gone || c->revoked;
}

/** The wait condition of a read: a whole packet is queued, or the open is
 * dead. */
static int evdev_readable(void *arg)
{
    struct evdev_client *c = (struct evdev_client *)arg;

    return c->packet != c->tail || dead(c);
}

/** The `n` records of `ev` as the reader takes them: as they are when they
 * are `size` bytes, the native size, and otherwise put into `ev32` with
 * their time in 32 bits, as Linux's input_event_to_user gives them to a
 * compat caller. Returns the records to copy. */
static const void *records(const struct input_event *ev, struct input_event32 *ev32, int n,
                           uint32_t size)
{
    int i;

    if (size == sizeof(*ev)) {
        return ev;
    }
    for (i = 0; i < n; i++) {
        ev32[i].sec = (uint32_t)ev[i].sec;
        ev32[i].usec = (uint32_t)ev[i].usec;
        ev32[i].type = ev[i].type;
        ev32[i].code = ev[i].code;
        ev32[i].value = ev[i].value;
    }
    return ev32;
}

int32_t evdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct input_event ev[8];
    struct input_event32 ev32[8];
    uint32_t size = sizeof(ev[0]);
    uint32_t done = 0;

    if (in_compat_syscall()) {
        size = sizeof(ev32[0]);
    }
    if (count < size) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t flags;
        int n = 0;

        flags = spin_lock_irqsave(&evdev_lock);
        if (dead(c)) {
            spin_unlock_irqrestore(&evdev_lock, flags);
            return done ? (int32_t)done : -ENODEV;
        }
        while (n < 8 && c->tail != c->packet && done + (n + 1) * size <= count) {
            ev[n++] = c->queue[c->tail];
            c->tail = (c->tail + 1) & c->qmask;
        }
        spin_unlock_irqrestore(&evdev_lock, flags);
        if (n > 0) {
            if (copy_to_user(buf + done, records(ev, ev32, n, size), n * size) != 0) {
                return -EFAULT;
            }
            done += n * size;
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
    if (dead(c)) {
        return POLLHUP | POLLERR;
    }
    return (short)(events & (POLLOUT | (c->packet != c->tail ? POLLIN : 0)));
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

/** Zero `len` bytes of the user's memory at `p`. Returns 0, or -EFAULT. */
static int32_t zero_out(char __user *p, uint32_t len)
{
    static const uint8_t zero[64];

    while (len > 0) {
        uint32_t n = len < sizeof(zero) ? len : (uint32_t)sizeof(zero);

        if (copy_to_user(p, zero, n) != 0) {
            return -EFAULT;
        }
        p += n;
        len -= n;
    }
    return 0;
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

/**
 * EVIOCREVOKE: the open `c` gets no more events, gives up its grab, and
 * reads, polls and ioctls as a device that has gone, for good, while the
 * device's other opens go on. A logind or seatd revokes a session's
 * descriptors when it switches away. -EINVAL for an argument other than 0,
 * as Linux's.
 */
static int32_t revoke(struct evdev_client *c, uintptr_t arg)
{
    uint32_t flags;

    if (arg) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    c->revoked = 1;
    if (c->dev->grab == c) {
        c->dev->grab = NULL;
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    wq_wake_all(&c->dev->wq);
    return 0;
}

/**
 * EVIOCSCLOCKID: stamp the records of the open `c` by the clock whose id the
 * int at `arg` holds, CLOCK_REALTIME, the default, CLOCK_MONOTONIC, which
 * libinput asks for, or CLOCK_BOOTTIME. A change of clock empties a queue
 * that holds records, which a SYN_DROPPED stamped now replaces, to be read
 * once the next packet ends, as Linux's does. -EINVAL for another clock,
 * -EFAULT.
 */
static int32_t set_clock(struct evdev_client *c, uintptr_t arg)
{
    struct input_event ev;
    int64_t sec[CLKS];
    int32_t nsec[CLKS];
    uint32_t flags;
    int id, clk;

    if (copy_from_user(&id, (const void __user *)arg, sizeof(id)) != 0) {
        return -EFAULT;
    }
    switch (id) {
    case CLOCK_REALTIME:
        clk = CLK_REAL;
        break;
    case CLOCK_MONOTONIC:
        clk = CLK_MONO;
        break;
    case CLOCK_BOOTTIME:
        clk = CLK_BOOT;
        break;
    default:
        return -EINVAL;
    }
    read_clocks(sec, nsec);
    flags = spin_lock_irqsave(&evdev_lock);
    if (c->clock != clk) {
        c->clock = clk;
        if (c->head != c->tail) {
            c->packet = c->head = c->tail;
            ev.sec = (unsigned long)sec[clk];
            ev.usec = (unsigned long)((uint32_t)nsec[clk] / 1000u);
            ev.type = EV_SYN;
            ev.code = SYN_DROPPED;
            ev.value = 0;
            c->queue[c->head] = ev;
            c->head = (c->head + 1) & c->qmask;
        }
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    return 0;
}

/**
 * EVIOCGMASK: copy the mask of the type the `struct input_mask` at `arg`
 * names into its buffer, a bit for each code, cut to the buffer's size and
 * the rest of the buffer zeroed; a type with no mask set reads all ones,
 * and one that takes no mask all zeros. -EFAULT.
 */
static int32_t get_mask(struct evdev_client *c, uintptr_t arg)
{
    uint8_t bits[BITS_BYTES(KEY_CNT)];
    struct input_mask m;
    char __user *codes;
    uint32_t size = 0;
    uint32_t flags;
    unsigned count;
    uint8_t *mask;

    if (copy_from_user(&m, (const void __user *)arg, sizeof(m)) != 0) {
        return -EFAULT;
    }
    codes = (char __user *)(uintptr_t)m.codes_ptr;
    mask = client_mask(c, m.type, &count);
    if (mask) {
        size = (uint32_t)BITS_BYTES(count);
        if (size > m.codes_size) {
            size = m.codes_size;
        }
        flags = spin_lock_irqsave(&evdev_lock);
        if (c->masked & (1u << m.type)) {
            k_memcpy(bits, mask, size);
        } else {
            k_memset(bits, 0xFF, size);
        }
        spin_unlock_irqrestore(&evdev_lock, flags);
        if (size > 0 && copy_to_user(codes, bits, size) != 0) {
            return -EFAULT;
        }
    }
    return zero_out(codes + size, m.codes_size - size);
}

/**
 * EVIOCSMASK: set the mask of the type the `struct input_mask` at `arg`
 * names from its buffer: the open `c` then gets only the codes of that type
 * whose bits are on, a code beyond the buffer's size being off; the mask of
 * EV_SYN chooses whole types, and SYN_REPORT always passes. A type that
 * takes no mask is ignored, as Linux ignores it. -EFAULT.
 */
static int32_t set_mask(struct evdev_client *c, uintptr_t arg)
{
    uint8_t bits[BITS_BYTES(KEY_CNT)];
    struct input_mask m;
    uint32_t size, flags;
    unsigned count;
    uint8_t *mask;

    if (copy_from_user(&m, (const void __user *)arg, sizeof(m)) != 0) {
        return -EFAULT;
    }
    mask = client_mask(c, m.type, &count);
    if (!mask) {
        return 0;
    }
    k_memset(bits, 0, sizeof(bits));
    size = (uint32_t)BITS_BYTES(count);
    if (size > m.codes_size) {
        size = m.codes_size;
    }
    if (size > 0 && copy_from_user(bits, (const void __user *)(uintptr_t)m.codes_ptr, size) != 0) {
        return -EFAULT;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    k_memcpy(mask, bits, BITS_BYTES(count));
    c->masked |= 1u << m.type;
    spin_unlock_irqrestore(&evdev_lock, flags);
    return 0;
}

/**
 * Store in `out`, of EVDEV_KEY_BYTES bytes, the bits EVIOCGBIT(type) gives
 * of `dev`: those of the event types it has, of its keys, of its relative
 * and absolute axes, of its MSC codes and of a keyboard's LEDs, and none of
 * the other types. Returns the bytes of the type's bits, each of its own
 * length, or -1 for a type Linux's evdev does not answer, EV_REP among them,
 * whose times EVIOCGREP gives. The caller holds the evdev lock.
 */
static int type_bytes(const struct evdev *dev, uint32_t type, uint8_t *out)
{
    unsigned i;

    k_memset(out, 0, EVDEV_KEY_BYTES);
    switch (type) {
    case 0:
        out[0] =
            (uint8_t)(1u << EV_SYN | 1u << EV_KEY | ((dev->flags & EVDEV_SCAN) ? 1u << EV_MSC : 0));
        for (i = 0; i < sizeof(dev->rel); i++) {
            if (dev->rel[i]) {
                out[0] |= 1u << EV_REL;
            }
        }
        if (dev->has_abs) {
            out[0] |= 1u << EV_ABS;
        }
        if (dev->flags & EVDEV_KEYBOARD) {
            out[EV_LED >> 3] |= (uint8_t)(1u << (EV_LED & 7));
            out[EV_REP >> 3] |= (uint8_t)(1u << (EV_REP & 7));
        }
        return (int)BITS_BYTES(EV_CNT);
    case EV_KEY:
        k_memcpy(out, dev->keys, EVDEV_KEY_BYTES);
        return EVDEV_KEY_BYTES;
    case EV_MSC:
        out[0] = (uint8_t)((dev->flags & EVDEV_SCAN) ? 1u << MSC_SCAN : 0);
        return (int)BITS_BYTES(MSC_CNT);
    case EV_REL:
        k_memcpy(out, dev->rel, sizeof(dev->rel));
        return (int)BITS_BYTES(REL_CNT);
    case EV_ABS:
        k_memcpy(out, dev->abs, sizeof(dev->abs));
        return (int)BITS_BYTES(ABS_CNT);
    case EV_SW:
        return (int)BITS_BYTES(SW_CNT);
    case EV_LED:
        if (dev->flags & EVDEV_KEYBOARD) {
            out[0] = 1u << LED_NUML | 1u << LED_CAPSL | 1u << LED_SCROLLL;
        }
        return (int)BITS_BYTES(LED_CNT);
    case EV_SND:
        return (int)BITS_BYTES(SND_CNT);
    case EV_FF:
        return (int)BITS_BYTES(FF_CNT);
    default:
        return -1;
    }
}

/**
 * Inject the event `type`, `code`, `value` a program wrote to `dev`, as
 * Linux's input_inject_event: an event of a type the device lacks is
 * dropped; a keyboard's EV_LED and EV_REP go to the LEDs and the repeat
 * every keyboard shares (`input_written`), whose change each keyboard's
 * opens get; any other event takes a driver's path (`evdev_report`), and
 * one that goes out to the opens with no grab held goes on as Linux's input
 * core gives an injected event to its other handlers: a keyboard's key to
 * the console (`input_written`), and a mouse's motion and buttons to the
 * mouse queue (`mouse_written`), unless the mouse has a handler of its own,
 * a touchpad's, which `evdev_report` gave it already.
 */
static void inject(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    uint8_t types[EVDEV_KEY_BYTES];
    uint32_t flags;
    int keyboard, pointer, grabbed;

    flags = spin_lock_irqsave(&evdev_lock);
    type_bytes(dev, 0, types);
    keyboard = (dev->flags & EVDEV_KEYBOARD) != 0;
    pointer = !keyboard && !dev->handler && bit_on(dev->rel, REL_X);
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (type >= EV_CNT || !bit_on(types, type)) {
        return;
    }
    if (type == EV_LED || type == EV_REP) {
        input_written(type, code, value);
        return;
    }
    if (!report(dev, type, code, value, &grabbed) || grabbed) {
        return;
    }
    if (type == EV_KEY && keyboard) {
        input_written(type, code, value);
    } else if (pointer) {
        mouse_written(type, code, value);
    }
}

int32_t evdev_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct input_event32 ev32;
    struct input_event ev;
    uint32_t size = sizeof(ev);
    uint32_t done = 0;

    if (in_compat_syscall()) {
        size = sizeof(ev32);
    }
    if (count > WRITE_MAX) {
        count = WRITE_MAX;
    }
    if (count != 0 && count < size) {
        return -EINVAL;
    }
    if (dead(c)) {
        return -ENODEV;
    }
    while (done + size <= count) {
        if (size == sizeof(ev)) {
            if (copy_from_user(&ev, buf + done, size) != 0) {
                return -EFAULT;
            }
        } else {
            if (copy_from_user(&ev32, buf + done, size) != 0) {
                return -EFAULT;
            }
            ev.type = ev32.type;
            ev.code = ev32.code;
            ev.value = ev32.value;
        }
        done += size;
        inject(c->dev, ev.type, ev.code, ev.value);
    }
    return (int32_t)done;
}

/** EVIOCGBIT(type): the bits `type_bytes` gives, cut to `len`; -EINVAL for
 * a type it does not answer. */
static int32_t type_bits(const struct evdev *dev, uint32_t type, uintptr_t arg, uint32_t len)
{
    uint8_t bits[EVDEV_KEY_BYTES];
    uint32_t flags = spin_lock_irqsave(&evdev_lock);
    int n = type_bytes(dev, type, bits);

    spin_unlock_irqrestore(&evdev_lock, flags);
    return n < 0 ? -EINVAL : bits_out(arg, len, bits, (uint32_t)n);
}

/** Copy the string `s`, or "" for NULL, into `out` of EVDEV_TEXT_MAX
 * bytes, cut to fit. */
static void copy_text(char *out, const char *s)
{
    int i = 0;

    while (s && s[i] && i < EVDEV_TEXT_MAX - 1) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

/** Store the bits of `type` of `dev` in `out`, of `cap` bytes, as
 * `type_bytes` gives them; the evdev lock is held. */
static void type_copy(const struct evdev *dev, uint32_t type, uint8_t *out, uint32_t cap)
{
    uint8_t bits[EVDEV_KEY_BYTES];
    int n = type_bytes(dev, type, bits);

    k_memset(out, 0, cap);
    k_memcpy(out, bits, n < 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap);
}

int evdev_snapshot(int n, struct evdev_desc *out)
{
    struct evdev *dev;
    uint32_t flags;
    int present;

    if (n < 0 || n >= EVDEV_MAX) {
        return 0;
    }
    dev = &devices[n];
    flags = spin_lock_irqsave(&evdev_lock);
    present = dev->used && !dev->gone;
    if (present) {
        out->gen = dev->gen;
        copy_text(out->name, dev->name);
        copy_text(out->phys, dev->phys);
        k_memcpy(out->id, dev->id, sizeof(out->id));
        type_copy(dev, 0, out->ev, sizeof(out->ev));
        type_copy(dev, EV_KEY, out->key, sizeof(out->key));
        type_copy(dev, EV_REL, out->rel, sizeof(out->rel));
        type_copy(dev, EV_ABS, out->abs, sizeof(out->abs));
        type_copy(dev, EV_MSC, out->msc, sizeof(out->msc));
        type_copy(dev, EV_LED, out->led, sizeof(out->led));
        type_copy(dev, EV_SND, out->snd, sizeof(out->snd));
        type_copy(dev, EV_FF, out->ff, sizeof(out->ff));
        type_copy(dev, EV_SW, out->sw, sizeof(out->sw));
        k_memset(out->prop, 0, sizeof(out->prop));
        k_memcpy(out->prop, dev->props, sizeof(dev->props));
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    return present;
}

uint32_t evdev_changes(void)
{
    uint32_t flags = spin_lock_irqsave(&evdev_lock);
    uint32_t n = change_count;

    spin_unlock_irqrestore(&evdev_lock, flags);
    return n;
}

struct wait_queue *evdev_change_queue(void)
{
    return &change_wq;
}

/**
 * Take the records of `type` out of the queue of `c`, with the evdev lock
 * held, as Linux's `__evdev_flush_queue` does when a client reads the state
 * of that type: the rest close up, a packet left empty loses its
 * SYN_REPORT, and the packet still coming stays unread.
 */
static void flush_type(struct evdev_client *c, uint16_t type)
{
    unsigned i, head = c->tail, n = 1;

    c->packet = c->tail;
    for (i = c->tail; i != c->head; i = (i + 1) & c->qmask) {
        struct input_event *ev = &c->queue[i];
        int syn = ev->type == EV_SYN && ev->code == SYN_REPORT;

        if (ev->type == type || (syn && n == 0)) {
            continue;
        }
        if (head != i) {
            c->queue[head] = *ev;
        }
        n++;
        head = (head + 1) & c->qmask;
        if (syn) {
            n = 0;
            c->packet = head;
        }
    }
    c->head = head;
}

/**
 * EVIOCGMTSLOTS(len): fill the int32_t after the code at `arg` with the
 * value of that multitouch axis in each slot of `dev`, as many as the slots
 * and the `len` bytes allow, as Linux's `evdev_handle_mt_request`. -EINVAL
 * for a device without slots or a code that is no multitouch value,
 * -EFAULT.
 */
static int32_t mt_slots(struct evdev *dev, uintptr_t arg, uint32_t len)
{
    int32_t values[EVDEV_MT_SLOTS];
    uint32_t code, flags;
    int i, n;

    if (copy_from_user(&code, (const void __user *)arg, sizeof(code)) != 0) {
        return -EFAULT;
    }
    if (!dev->num_slots || code < ABS_MT_FIRST || code > ABS_MT_LAST) {
        return -EINVAL;
    }
    n = len < sizeof(code) ? 0 : (int)((len - sizeof(code)) / sizeof(int32_t));
    if (n > dev->num_slots) {
        n = dev->num_slots;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    for (i = 0; i < n; i++) {
        values[i] = dev->slots[i].abs[code - ABS_MT_FIRST];
    }
    spin_unlock_irqrestore(&evdev_lock, flags);
    if (n > 0 && copy_to_user((char __user *)arg + sizeof(code), values,
                              (uint32_t)n * sizeof(int32_t)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The ioctls that read a string, a set of bits or an axis, numbered `nr`
 * with the caller's length `len`: the name, place and serial, the
 * properties, the slots' values (EVIOCGMTSLOTS), the keys down, a
 * keyboard's LEDs, the sounds and switches, which no device here has,
 * EVIOCGBIT and EVIOCGABS. Reading the keys or
 * the LEDs takes their records out of the queue of `c`, since the state read
 * includes them. -EINVAL for another number, and for EVIOCGABS on a device
 * with no absolute axis.
 */
static int32_t read_ioctl(struct evdev_client *c, uint32_t nr, uintptr_t arg, uint32_t len)
{
    struct evdev *dev = c->dev;
    uint8_t down[EVDEV_KEY_BYTES];
    uint8_t leds[sizeof(long)];
    struct input_absinfo a;
    uint32_t flags;

    if (nr >= NR_GBIT && nr < NR_GBIT + EV_CNT) {
        return type_bits(dev, nr - NR_GBIT, arg, len);
    }
    if (nr >= NR_GABS && nr < NR_GABS + ABS_CNT) {
        if (!dev->has_abs) {
            return -EINVAL;
        }
        flags = spin_lock_irqsave(&evdev_lock);
        a = dev->absinfo[nr - NR_GABS];
        spin_unlock_irqrestore(&evdev_lock, flags);
        if (len > sizeof(a)) {
            len = sizeof(a);
        }
        return copy_to_user((void __user *)arg, &a, len) != 0 ? -EFAULT : 0;
    }
    switch (nr) {
    case NR_GNAME:
        return string_out(arg, len, dev->name);
    case NR_GPHYS:
        return string_out(arg, len, dev->phys);
    case NR_GUNIQ:
        return string_out(arg, len, NULL);
    case NR_GPROP:
        return bits_out(arg, len, dev->props, BITS_BYTES(INPUT_PROP_CNT));
    case NR_GMTSLOTS:
        return mt_slots(dev, arg, len);
    case NR_GKEY:
        flags = spin_lock_irqsave(&evdev_lock);
        k_memcpy(down, dev->down, sizeof(down));
        flush_type(c, EV_KEY);
        spin_unlock_irqrestore(&evdev_lock, flags);
        return bits_out(arg, len, down, EVDEV_KEY_BYTES);
    case NR_GLED:
        k_memset(leds, 0, sizeof(leds));
        flags = spin_lock_irqsave(&evdev_lock);
        leds[0] = (uint8_t)dev->leds;
        flush_type(c, EV_LED);
        spin_unlock_irqrestore(&evdev_lock, flags);
        return bits_out(arg, len, leds, BITS_BYTES(LED_CNT));
    case NR_GSND:
        return bits_out(arg, len, NULL, BITS_BYTES(SND_CNT));
    case NR_GSW:
        return bits_out(arg, len, NULL, BITS_BYTES(SW_CNT));
    default:
        return -EINVAL;
    }
}

/**
 * EVIOCSABS(axis): set the state and limits of the absolute axis `axis` of
 * `dev` from the `struct input_absinfo` at `arg`, cut to `len` bytes, its
 * resolution 0 when the caller's structure is shorter than Linux's. -EINVAL
 * on a device with no absolute axis and for ABS_MT_SLOT, whose count of
 * slots does not change, -EFAULT.
 */
static int32_t set_abs(struct evdev *dev, uint32_t axis, uintptr_t arg, uint32_t len)
{
    struct input_absinfo a;
    uint32_t flags;

    if (!dev->has_abs) {
        return -EINVAL;
    }
    k_memset(&a, 0, sizeof(a));
    if (len > sizeof(a)) {
        len = sizeof(a);
    }
    if (copy_from_user(&a, (const void __user *)arg, len) != 0) {
        return -EFAULT;
    }
    if (len < sizeof(a)) {
        a.resolution = 0;
    }
    if (axis == ABS_MT_SLOT) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&evdev_lock);
    dev->absinfo[axis] = a;
    spin_unlock_irqrestore(&evdev_lock, flags);
    return 0;
}

/**
 * EVIOCGREP and EVIOCSREP: read into, or set from, the two unsigned ints at
 * `arg` the repeat's delay and period in milliseconds, which every keyboard
 * shares (`input_set_repeat`). Setting a time above INT_MAX leaves it as it
 * is, as Linux's input core ignores a negative one, and a period of 0 turns
 * repeat off. -ENOSYS for a device that does not repeat, which has no
 * EV_REP: only the keyboards, EVDEV_KEYBOARD, do.
 */
static int32_t repeat_ioctl(const struct evdev *dev, uint32_t request, uintptr_t arg)
{
    unsigned int rep[2];
    int delay, period;

    if (!(dev->flags & EVDEV_KEYBOARD)) {
        return -ENOSYS;
    }
    if (request == EVIOCGREP) {
        input_get_repeat(&delay, &period);
        rep[0] = (unsigned int)delay;
        rep[1] = (unsigned int)period;
        return copy_to_user((void __user *)arg, rep, sizeof(rep)) != 0 ? -EFAULT : 0;
    }
    if (copy_from_user(rep, (const void __user *)arg, sizeof(rep)) != 0) {
        return -EFAULT;
    }
    input_set_repeat((int)rep[0], (int)rep[1]);
    return 0;
}

int32_t evdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct evdev_client *c = (struct evdev_client *)e->priv;
    struct evdev *dev = c->dev;
    int version = EV_VERSION;

    if (dead(c)) {
        return -ENODEV;
    }
    switch (request) {
    case EVIOCGVERSION:
        return copy_to_user((void __user *)arg, &version, sizeof(version)) != 0 ? -EFAULT : 0;
    case EVIOCGID:
        return copy_to_user((void __user *)arg, dev->id, sizeof(dev->id)) != 0 ? -EFAULT : 0;
    case EVIOCGREP:
    case EVIOCSREP:
        return repeat_ioctl(dev, request, arg);
    case EVIOCGRAB:
        return grab(c, arg);
    case EVIOCREVOKE:
        return revoke(c, arg);
    case EVIOCGMASK:
        return get_mask(c, arg);
    case EVIOCSMASK:
        return set_mask(c, arg);
    case EVIOCSCLOCKID:
        return set_clock(c, arg);
    default:
        break;
    }
    if (IOC_TYPE(request) != 'E') {
        return -EINVAL;
    }
    if (IOC_DIR(request) == IOC_READ) {
        return read_ioctl(c, IOC_NR(request), arg, IOC_SIZE(request));
    }
    if (IOC_DIR(request) == IOC_WRITE && IOC_NR(request) >= NR_SABS) {
        return set_abs(dev, IOC_NR(request) - NR_SABS, arg, IOC_SIZE(request));
    }
    return -EINVAL;
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
    kfree(c->queue);
    kfree(c);
}
