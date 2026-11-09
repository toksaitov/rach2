/*
 * evdev_hosttest.c: the event devices of rach2/sys/evdev.c, a mouse's and a
 * touchpad's, read and asked as Linux's evdev answers: their records and
 * packets, the input core's filters, an overflowed queue, revoke, the
 * clocks of EVIOCSCLOCKID against clock_gettime, the event masks, the
 * absolute axes, the 16-byte records of an i386 reader, and the records a
 * program writes.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The file is included whole. A user pointer is a pointer of the test's,
 * and one below 4096 faults. The clocks are the host's, read through
 * clock_gettime, or fixed times a row sets. Every open is nonblocking, so
 * no read sleeps.
 */

#include "evdev.c"

int printf(const char *format, ...);
int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                           \
    do {                                            \
        if (!(cond)) {                              \
            printf("evdev-test: fail: %s\n", what); \
            fails++;                                \
        }                                           \
    } while (0)

/* The host's clocks, as its clock_gettime numbers them. */
#define HOST_CLOCK_REALTIME 0
#ifdef __APPLE__
#define HOST_CLOCK_MONOTONIC 6
#else
#define HOST_CLOCK_MONOTONIC 1
#endif

/* The host's `struct timespec` on the 64-bit hosts the tests run on. */
struct host_timespec {
    long tv_sec;
    long tv_nsec;
};

int clock_gettime(int clock, struct host_timespec *ts);

/* Fixed clocks: while `fixed` is set, the real time and the monotonic
 * clock read these, in nanoseconds. */
static int fixed;
static int64_t fixed_real;
static int64_t fixed_mono;

/** No interrupts to mask on the host: nothing to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** Read the host's clock `clock`, or the fixed time `ns`. */
static void read_clock(int clock, int64_t ns, int64_t *seconds, int64_t *nanoseconds)
{
    struct host_timespec ts;

    if (fixed) {
        *seconds = ns / 1000000000;
        *nanoseconds = ns % 1000000000;
        return;
    }
    clock_gettime(clock, &ts);
    *seconds = ts.tv_sec;
    *nanoseconds = ts.tv_nsec;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    read_clock(HOST_CLOCK_REALTIME, fixed_real, seconds, nanoseconds);
}

void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    read_clock(HOST_CLOCK_MONOTONIC, fixed_mono, seconds, nanoseconds);
}

/** No keyboard here repeats. */
void input_get_repeat(int *delay_ms, int *period_ms)
{
    *delay_ms = 250;
    *period_ms = 33;
}

/** Nothing to set. */
void input_set_repeat(int delay_ms, int period_ms)
{
    (void)delay_ms;
    (void)period_ms;
}

/* The events evdev.c gave input.c as written to a keyboard, the last of
 * them, and their count. */
static uint16_t written_type, written_code;
static int32_t written_value;
static int n_written;

/** Record an event written to a keyboard. */
void input_written(uint16_t type, uint16_t code, int32_t value)
{
    written_type = type;
    written_code = code;
    written_value = value;
    n_written++;
}

/* The wakes of every queue. */
static int wakes;

void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

/** A read never sleeps here: every open is nonblocking. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/* Whether the calls are an i386 program's on the x86-64 kernel, which a row
 * sets. */
static int compat_caller;

/** The convention of the call: an i386 program's while a row says so. */
int in_compat_syscall(void)
{
    return compat_caller;
}

/* A key of the keyboard the write rows register. */
#define KEY_A 30

/* The ioctls, by Linux's macros. */
#define IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('E' << 8) | (nr))
#define EVIOCGNAME(n)      IOC(IOC_READ, NR_GNAME, n)
#define EVIOCGPHYS(n)      IOC(IOC_READ, NR_GPHYS, n)
#define EVIOCGUNIQ(n)      IOC(IOC_READ, NR_GUNIQ, n)
#define EVIOCGPROP(n)      IOC(IOC_READ, NR_GPROP, n)
#define EVIOCGKEY(n)       IOC(IOC_READ, NR_GKEY, n)
#define EVIOCGBIT(t, n)    IOC(IOC_READ, NR_GBIT + (t), n)
#define EVIOCGABS(a)       IOC(IOC_READ, NR_GABS + (a), sizeof(struct input_absinfo))
#define EVIOCSABS(a, n)    IOC(IOC_WRITE, NR_SABS + (a), n)

/* An open: its descriptor and description, nonblocking. */
struct opened {
    fd_entry_t e;
    struct open_file ofd;
};

/** Open device `n` as `o`; returns what `evdev_open` returns. */
static int32_t open_dev(struct opened *o, int n)
{
    k_memset(o, 0, sizeof(*o));
    o->ofd.status_flags = OFD_NONBLOCK;
    o->e.ofd = &o->ofd;
    return evdev_open(&o->e, n);
}

/* The records the last `take` read. */
static struct input_event got[EVDEV_QUEUE];
static int n_got;

/** Read every whole packet of `o` into `got`; returns the read's result. */
static int32_t take(struct opened *o)
{
    int32_t rc = evdev_read(&o->e, (char __user *)got, sizeof(got));

    n_got = rc > 0 ? rc / (int32_t)sizeof(got[0]) : 0;
    return rc;
}

/** Whether record `i` of `got` is `type`, `code`, `value`. */
static int is(int i, uint16_t type, uint16_t code, int32_t value)
{
    return i < n_got && got[i].type == type && got[i].code == code && got[i].value == value;
}

/** Report a packet of one event and SYN_REPORT. */
static void packet(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    evdev_report(dev, type, code, value);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/** Register a mouse with a wheel, as mouse.c's `mouse_register` makes it. */
static struct evdev *mouse(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct evdev *dev;

    k_memset(keys, 0, sizeof(keys));
    dev = evdev_register("ImPS/2 Generic Wheel Mouse", EVDEV_BUS_I8042, 2, 5, keys);
    evdev_describe(dev, "isa0060/serio1/input0", 0);
    evdev_set_capability(dev, EV_KEY, BTN_LEFT);
    evdev_set_capability(dev, EV_KEY, BTN_RIGHT);
    evdev_set_capability(dev, EV_REL, REL_X);
    evdev_set_capability(dev, EV_REL, REL_Y);
    evdev_set_capability(dev, EV_REL, REL_WHEEL);
    evdev_set_prop(dev, INPUT_PROP_POINTER);
    return dev;
}

/** A mouse's capabilities, records and packets, the filters and an
 * overflow. */
static void motion_rows(void)
{
    struct evdev *dev = mouse();
    uint8_t keys[EVDEV_KEY_BYTES];
    struct opened a, b;
    uint8_t bits[16];
    char name[64];
    int i;

    CHECK("a mouse registers", dev == &devices[0]);
    CHECK("two opens", open_dev(&a, 0) == 0 && open_dev(&b, 0) == 0);
    k_memset(bits, 0xAA, sizeof(bits));
    CHECK("EVIOCGBIT(0) gives a long",
          evdev_ioctl(&a.e, EVIOCGBIT(0, 16), (uintptr_t)bits) == (int32_t)sizeof(long));
    CHECK("its types are EV_SYN, EV_KEY and EV_REL", bits[0] == 0x07 && bits[1] == 0);
    evdev_ioctl(&a.e, EVIOCGBIT(EV_REL, 16), (uintptr_t)bits);
    CHECK("its axes are REL_X, REL_Y and REL_WHEEL", bits[0] == 0x03 && bits[1] == 0x01);
    CHECK("EVIOCGBIT(EV_KEY) gives 96 bytes",
          evdev_ioctl(&a.e, EVIOCGBIT(EV_KEY, 96), (uintptr_t)keys) == EVDEV_KEY_BYTES);
    CHECK("its buttons are BTN_LEFT and BTN_RIGHT",
          keys[BTN_LEFT >> 3] == 0x03 && keys[0] == 0 && keys[(BTN_LEFT >> 3) - 1] == 0);
    CHECK("EVIOCGPROP has INPUT_PROP_POINTER",
          evdev_ioctl(&a.e, EVIOCGPROP(4), (uintptr_t)bits) == 4 && bits[0] == 0x01);
    CHECK("EVIOCGNAME gives the name and its terminator",
          evdev_ioctl(&a.e, EVIOCGNAME(64), (uintptr_t)name) == 27 &&
              strcmp(name, "ImPS/2 Generic Wheel Mouse") == 0);
    CHECK("EVIOCGPHYS gives the place", evdev_ioctl(&a.e, EVIOCGPHYS(64), (uintptr_t)name) == 22 &&
                                            strcmp(name, "isa0060/serio1/input0") == 0);
    CHECK("EVIOCGUNIQ is -ENOENT", evdev_ioctl(&a.e, EVIOCGUNIQ(64), (uintptr_t)name) == -ENOENT);
    CHECK("EVIOCGABS of a device with no absolute axis is -EINVAL",
          evdev_ioctl(&a.e, EVIOCGABS(ABS_X), (uintptr_t)name) == -EINVAL);
    CHECK("an ioctl evdev does not know is -EINVAL",
          evdev_ioctl(&a.e, IOC(IOC_WRITE, 0x50, 4), (uintptr_t)name) == -EINVAL &&
              evdev_ioctl(&a.e, 0x5401u, (uintptr_t)name) == -EINVAL);

    /* One motion: both opens read the packet. */
    evdev_report(dev, EV_REL, REL_X, 5);
    evdev_report(dev, EV_REL, REL_Y, -3);
    CHECK("a packet still coming is not read", take(&a) == -EAGAIN);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    CHECK("a motion is read as its packet", take(&a) == 3 * (int32_t)sizeof(got[0]));
    CHECK("REL_X, REL_Y and SYN_REPORT",
          is(0, EV_REL, REL_X, 5) && is(1, EV_REL, REL_Y, -3) && is(2, EV_SYN, SYN_REPORT, 0));
    CHECK("the other open reads it too", take(&b) > 0 && n_got == 3 && is(0, EV_REL, REL_X, 5));

    /* The input core's filters. */
    evdev_report(dev, EV_REL, REL_X, 0);
    evdev_report(dev, EV_REL, REL_HWHEEL, 1);
    evdev_report(dev, EV_KEY, BTN_MIDDLE, 1);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    CHECK("a motion of 0 and codes the mouse lacks make no packet", take(&a) == -EAGAIN);
    packet(dev, EV_KEY, BTN_LEFT, 1);
    packet(dev, EV_KEY, BTN_LEFT, 1);
    packet(dev, EV_KEY, BTN_LEFT, 0);
    packet(dev, EV_KEY, BTN_LEFT, 0);
    CHECK("a button pressed while down and released while up is dropped",
          take(&a) > 0 && n_got == 4 && is(0, EV_KEY, BTN_LEFT, 1) &&
              is(1, EV_SYN, SYN_REPORT, 0) && is(2, EV_KEY, BTN_LEFT, 0));
    take(&b);

    /* An overflow: the reader that fell behind finds SYN_DROPPED. A mouse
     * with three axes queues 128 records. */
    CHECK("a mouse's open queues 128 records", ((struct evdev_client *)a.e.priv)->qmask + 1 == 128);
    for (i = 0; i < 128 / 2 + 1; i++) {
        packet(dev, EV_REL, REL_WHEEL, 1);
    }
    CHECK("an overflowed queue reads SYN_DROPPED first",
          take(&a) > 0 && is(0, EV_SYN, SYN_DROPPED, 0));
    CHECK("then the newest record it held, and the packet after",
          n_got == 4 && is(1, EV_SYN, SYN_REPORT, 0) && is(2, EV_REL, REL_WHEEL, 1) &&
              is(3, EV_SYN, SYN_REPORT, 0));
    CHECK("the queue is empty after", take(&a) == -EAGAIN);
    CHECK("a read too small for a record is -EINVAL",
          evdev_read(&b.e, (char __user *)got, sizeof(got[0]) - 1) == -EINVAL);
    evdev_close(&a.e);
    evdev_close(&b.e);
    evdev_unregister(dev);
}

/** EVIOCREVOKE: a revoked open reads -ENODEV while the others read on. */
static void revoke_rows(void)
{
    struct evdev *dev = mouse();
    struct wait_queue *wq;
    struct opened a, b;
    int version;

    open_dev(&a, 0);
    open_dev(&b, 0);
    CHECK("a revoke with an argument is -EINVAL", evdev_ioctl(&b.e, EVIOCREVOKE, 1) == -EINVAL);
    CHECK("the grab is taken", evdev_ioctl(&b.e, EVIOCGRAB, 1) == 0);
    packet(dev, EV_REL, REL_X, 1);
    CHECK("the grab's holder alone reads", take(&a) == -EAGAIN && take(&b) > 0);
    CHECK("a revoke succeeds", evdev_ioctl(&b.e, EVIOCREVOKE, 0) == 0);
    packet(dev, EV_REL, REL_X, 2);
    CHECK("a revoked open reads -ENODEV", take(&b) == -ENODEV);
    CHECK("its ioctls are -ENODEV",
          evdev_ioctl(&b.e, EVIOCGVERSION, (uintptr_t)&version) == -ENODEV);
    CHECK("its poll says hang-up", evdev_poll(&b.e, POLLIN, &wq) == (POLLHUP | POLLERR));
    CHECK("its grab went with the revoke", take(&a) > 0 && is(0, EV_REL, REL_X, 2));
    CHECK("the other open polls as before", evdev_poll(&a.e, POLLIN, &wq) == 0 && wq == &dev->wq);
    packet(dev, EV_REL, REL_X, 3);
    CHECK("and reads the next packet", take(&a) > 0 && is(0, EV_REL, REL_X, 3));
    CHECK("and may grab the device", evdev_ioctl(&a.e, EVIOCGRAB, 1) == 0);
    evdev_close(&a.e);
    evdev_close(&b.e);
    evdev_unregister(dev);
}

/** The time of a record in microseconds. */
static int64_t usec_of(const struct input_event *ev)
{
    return (int64_t)ev->sec * 1000000 + (int64_t)ev->usec;
}

/** A host clock now in microseconds, cut as evdev cuts a record's time. */
static int64_t host_usec(int clock)
{
    struct host_timespec ts;

    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/** EVIOCSCLOCKID: each open's records on its clock, one time a packet, and
 * the queue emptied by a change. */
static void clock_rows(void)
{
    struct evdev *dev = mouse();
    struct opened real, mono, boot;
    int64_t before, after;
    int id;

    open_dev(&real, 0);
    open_dev(&mono, 0);
    open_dev(&boot, 0);
    id = 5;
    CHECK("an unknown clock is -EINVAL",
          evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id) == -EINVAL);
    CHECK("a bad pointer is -EFAULT", evdev_ioctl(&mono.e, EVIOCSCLOCKID, 8) == -EFAULT);
    id = CLOCK_MONOTONIC;
    CHECK("CLOCK_MONOTONIC is taken", evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id) == 0);
    id = CLOCK_BOOTTIME;
    CHECK("CLOCK_BOOTTIME is taken", evdev_ioctl(&boot.e, EVIOCSCLOCKID, (uintptr_t)&id) == 0);

    /* Against the host's clock_gettime. */
    before = host_usec(HOST_CLOCK_MONOTONIC);
    packet(dev, EV_REL, REL_X, 1);
    after = host_usec(HOST_CLOCK_MONOTONIC);
    take(&real);
    CHECK("a monotonic stamp lies between the clock_gettime calls around it",
          take(&mono) > 0 && usec_of(&got[0]) >= before && usec_of(&got[0]) <= after);
    CHECK("boot time reads the monotonic clock",
          take(&boot) > 0 && usec_of(&got[0]) >= before && usec_of(&got[0]) <= after);
    before = host_usec(HOST_CLOCK_REALTIME);
    packet(dev, EV_REL, REL_X, 1);
    after = host_usec(HOST_CLOCK_REALTIME);
    take(&real);
    CHECK("the default is the real time",
          n_got == 2 && usec_of(&got[1]) >= before && usec_of(&got[1]) <= after);
    take(&mono);
    take(&boot);

    /* Fixed clocks: one time for a whole packet, on each open's clock. */
    fixed = 1;
    fixed_real = 1700000000LL * 1000000000 + 123456789;
    fixed_mono = 42LL * 1000000000 + 987654321;
    evdev_report(dev, EV_REL, REL_X, 1);
    fixed_real += 5000000;
    fixed_mono += 5000000;
    evdev_report(dev, EV_REL, REL_Y, 1);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    take(&real);
    CHECK("a packet has its first event's real time",
          n_got == 3 && got[0].sec == 1700000000 && got[0].usec == 123456 &&
              usec_of(&got[1]) == usec_of(&got[0]) && usec_of(&got[2]) == usec_of(&got[0]));
    take(&mono);
    CHECK("and its monotonic time on a monotonic open", n_got == 3 && got[0].sec == 42 &&
                                                            got[0].usec == 987654 &&
                                                            usec_of(&got[2]) == usec_of(&got[0]));
    packet(dev, EV_REL, REL_X, 1);
    take(&mono);
    CHECK("the next packet has a time of its own", got[0].sec == 42 && got[0].usec == 992654);

    /* A change of clock empties a queue that holds records. */
    packet(dev, EV_REL, REL_X, 7);
    fixed_mono += 1000000;
    id = CLOCK_REALTIME;
    evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id);
    CHECK("the records queued before the change are gone", take(&mono) == -EAGAIN);
    packet(dev, EV_REL, REL_X, 8);
    take(&mono);
    CHECK("a SYN_DROPPED comes before the next packet",
          n_got == 3 && is(0, EV_SYN, SYN_DROPPED, 0) && is(1, EV_REL, REL_X, 8));
    CHECK("stamped by the new clock", got[0].sec == 1700000000 && got[1].sec == 1700000000);
    id = CLOCK_REALTIME;
    evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id);
    packet(dev, EV_REL, REL_X, 9);
    evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id);
    CHECK("the same clock again keeps the queue", take(&mono) > 0 && is(0, EV_REL, REL_X, 9));
    id = CLOCK_MONOTONIC;
    evdev_ioctl(&mono.e, EVIOCSCLOCKID, (uintptr_t)&id);
    packet(dev, EV_REL, REL_X, 10);
    CHECK("a change on an empty queue queues nothing",
          take(&mono) > 0 && n_got == 2 && is(0, EV_REL, REL_X, 10));
    fixed = 0;
    evdev_close(&real.e);
    evdev_close(&mono.e);
    evdev_close(&boot.e);
    evdev_unregister(dev);
}

/** EVIOCGMASK and EVIOCSMASK. */
static void mask_rows(void)
{
    struct evdev *dev = mouse();
    struct input_mask m;
    uint8_t codes[16];
    struct opened a;
    unsigned i;
    int ok;

    open_dev(&a, 0);
    m.type = EV_REL;
    m.codes_size = sizeof(codes);
    m.codes_ptr = (uint64_t)(uintptr_t)codes;
    k_memset(codes, 0x55, sizeof(codes));
    CHECK("EVIOCGMASK succeeds", evdev_ioctl(&a.e, EVIOCGMASK, (uintptr_t)&m) == 0);
    ok = 1;
    for (i = 0; i < sizeof(codes); i++) {
        ok &= codes[i] == (i < sizeof(long) ? 0xFF : 0);
    }
    CHECK("a type with no mask reads all ones, and zeros past its codes", ok);

    k_memset(codes, 0, sizeof(codes));
    codes[0] = 1u << REL_Y;
    m.codes_size = 1;
    CHECK("EVIOCSMASK succeeds", evdev_ioctl(&a.e, EVIOCSMASK, (uintptr_t)&m) == 0);
    evdev_report(dev, EV_REL, REL_X, 1);
    evdev_report(dev, EV_REL, REL_Y, 2);
    evdev_report(dev, EV_REL, REL_WHEEL, 1);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    CHECK("only the codes the mask lets through are read",
          take(&a) > 0 && n_got == 2 && is(0, EV_REL, REL_Y, 2) && is(1, EV_SYN, SYN_REPORT, 0));
    packet(dev, EV_REL, REL_X, 1);
    CHECK("a packet the mask empties is not read, nor its SYN_REPORT", take(&a) == -EAGAIN);
    k_memset(codes, 0x55, sizeof(codes));
    m.codes_size = sizeof(codes);
    evdev_ioctl(&a.e, EVIOCGMASK, (uintptr_t)&m);
    CHECK("EVIOCGMASK reads the mask back", codes[0] == 1u << REL_Y && codes[1] == 0);

    k_memset(codes, 0, sizeof(codes));
    codes[0] = 1u << EV_REL;
    m.type = EV_SYN;
    m.codes_size = 4;
    evdev_ioctl(&a.e, EVIOCSMASK, (uintptr_t)&m);
    packet(dev, EV_KEY, BTN_LEFT, 1);
    CHECK("the mask of EV_SYN leaves out whole types", take(&a) == -EAGAIN);
    packet(dev, EV_REL, REL_Y, 4);
    CHECK("and lets the others through", take(&a) > 0 && is(0, EV_REL, REL_Y, 4));

    m.type = 0x1F;
    k_memset(codes, 0x55, sizeof(codes));
    CHECK("a type that takes no mask is set without error",
          evdev_ioctl(&a.e, EVIOCSMASK, (uintptr_t)&m) == 0);
    CHECK("and reads as zeros", evdev_ioctl(&a.e, EVIOCGMASK, (uintptr_t)&m) == 0 &&
                                    codes[0] == 0 && codes[3] == 0 && codes[4] == 0x55);
    m.type = EV_REL;
    m.codes_ptr = 8;
    CHECK("a bad buffer is -EFAULT", evdev_ioctl(&a.e, EVIOCGMASK, (uintptr_t)&m) == -EFAULT &&
                                         evdev_ioctl(&a.e, EVIOCSMASK, (uintptr_t)&m) == -EFAULT);
    CHECK("a bad argument is -EFAULT", evdev_ioctl(&a.e, EVIOCGMASK, 8) == -EFAULT);
    evdev_close(&a.e);
    evdev_unregister(dev);
}

/** EVIOCGKEY: the keys down, and their records taken out of the queue. */
static void key_state_rows(void)
{
    struct evdev *dev = mouse();
    uint8_t keys[EVDEV_KEY_BYTES];
    struct opened a;

    open_dev(&a, 0);
    packet(dev, EV_REL, REL_X, 1);
    packet(dev, EV_KEY, BTN_LEFT, 1);
    evdev_report(dev, EV_KEY, BTN_RIGHT, 1);
    evdev_report(dev, EV_REL, REL_Y, 1);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    CHECK("EVIOCGKEY gives the buttons down",
          evdev_ioctl(&a.e, EVIOCGKEY(sizeof(keys)), (uintptr_t)keys) == (int32_t)sizeof(keys) &&
              (keys[BTN_LEFT >> 3] & 0x03) == 0x03);
    take(&a);
    CHECK("and takes their records out of the queue, with the packets left empty",
          n_got == 4 && is(0, EV_REL, REL_X, 1) && is(1, EV_SYN, SYN_REPORT, 0) &&
              is(2, EV_REL, REL_Y, 1) && is(3, EV_SYN, SYN_REPORT, 0));
    evdev_close(&a.e);
    evdev_unregister(dev);
}

/** A touchpad's absolute axes: EVIOCGABS, EVIOCSABS and the fuzz. */
static void abs_rows(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct input_absinfo info;
    struct evdev *dev;
    struct opened a;
    uint8_t bits[16];

    k_memset(keys, 0, sizeof(keys));
    dev = evdev_register("pad", EVDEV_BUS_I8042, 2, 7, keys);
    evdev_set_abs(dev, ABS_X, 1472, 5472, 4, 0);
    evdev_set_abs_res(dev, ABS_X, 42);
    evdev_set_abs(dev, ABS_Y, 1408, 4448, 4, 0);
    evdev_set_abs(dev, ABS_MT_SLOT, 0, 1, 0, 0);
    evdev_set_abs(dev, ABS_MT_POSITION_X, 1472, 5472, 0, 0);
    evdev_set_prop(dev, INPUT_PROP_BUTTONPAD);
    open_dev(&a, 0);

    evdev_ioctl(&a.e, EVIOCGBIT(0, 4), (uintptr_t)bits);
    CHECK("a touchpad has EV_ABS", bits[0] & (1u << EV_ABS));
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&a.e, EVIOCGBIT(EV_ABS, 8), (uintptr_t)bits);
    CHECK("EVIOCGBIT(EV_ABS) gives its axes",
          bits[0] == 0x03 && bits[5] == 0x80 && bits[6] == 0x20);
    CHECK("EVIOCGPROP has INPUT_PROP_BUTTONPAD",
          evdev_ioctl(&a.e, EVIOCGPROP(4), (uintptr_t)bits) == 4 && bits[0] == 0x04);
    CHECK("EVIOCGABS succeeds", evdev_ioctl(&a.e, EVIOCGABS(ABS_X), (uintptr_t)&info) == 0);
    CHECK("with the axis's limits", info.value == 0 && info.minimum == 1472 &&
                                        info.maximum == 5472 && info.fuzz == 4 && info.flat == 0 &&
                                        info.resolution == 42);
    CHECK("an axis the device lacks reads zeros",
          evdev_ioctl(&a.e, EVIOCGABS(ABS_PRESSURE), (uintptr_t)&info) == 0 && info.maximum == 0);
    CHECK("a bad pointer is -EFAULT", evdev_ioctl(&a.e, EVIOCGABS(ABS_X), 8) == -EFAULT);

    /* The fuzz: within half of it a value is the old one, within it a
     * quarter of the way, within twice it half. */
    packet(dev, EV_ABS, ABS_X, 2000);
    CHECK("a value far off passes", take(&a) > 0 && is(0, EV_ABS, ABS_X, 2000));
    packet(dev, EV_ABS, ABS_X, 2001);
    packet(dev, EV_ABS, ABS_X, 2003);
    CHECK("values within the fuzz that settle on the old one are dropped", take(&a) == -EAGAIN);
    packet(dev, EV_ABS, ABS_X, 2006);
    CHECK("a value within twice the fuzz is halved", take(&a) > 0 && is(0, EV_ABS, ABS_X, 2003));
    evdev_ioctl(&a.e, EVIOCGABS(ABS_X), (uintptr_t)&info);
    CHECK("EVIOCGABS reads the last value", info.value == 2003);
    packet(dev, EV_ABS, ABS_MT_SLOT, 1);
    packet(dev, EV_ABS, ABS_MT_POSITION_X, 3000);
    packet(dev, EV_ABS, ABS_MT_POSITION_X, 3000);
    CHECK("with no slots ABS_MT_SLOT is dropped and the multitouch axes pass unfiltered",
          take(&a) > 0 && n_got == 4 && is(0, EV_ABS, ABS_MT_POSITION_X, 3000) &&
              is(2, EV_ABS, ABS_MT_POSITION_X, 3000));
    packet(dev, EV_ABS, ABS_PRESSURE, 30);
    CHECK("an axis the device lacks is dropped", take(&a) == -EAGAIN);

    info.value = 0;
    info.minimum = 0;
    info.maximum = 100;
    info.fuzz = 0;
    info.flat = 0;
    info.resolution = 9;
    CHECK("EVIOCSABS succeeds",
          evdev_ioctl(&a.e, EVIOCSABS(ABS_Y, sizeof(info)), (uintptr_t)&info) == 0);
    k_memset(&info, 0, sizeof(info));
    evdev_ioctl(&a.e, EVIOCGABS(ABS_Y), (uintptr_t)&info);
    CHECK("and sets the axis", info.maximum == 100 && info.resolution == 9);
    info.resolution = 9;
    CHECK("a shorter structure sets no resolution",
          evdev_ioctl(&a.e, EVIOCSABS(ABS_Y, 20), (uintptr_t)&info) == 0 &&
              evdev_ioctl(&a.e, EVIOCGABS(ABS_Y), (uintptr_t)&info) == 0 && info.resolution == 0 &&
              info.maximum == 100);
    CHECK("ABS_MT_SLOT's count does not change",
          evdev_ioctl(&a.e, EVIOCSABS(ABS_MT_SLOT, sizeof(info)), (uintptr_t)&info) == -EINVAL);
    CHECK("a bad pointer is -EFAULT",
          evdev_ioctl(&a.e, EVIOCSABS(ABS_Y, sizeof(info)), 8) == -EFAULT);
    evdev_close(&a.e);
    evdev_unregister(dev);
}

/** Write the `n` records `ev` to `o` in the native layout; returns what the
 * write returns. */
static int32_t put(struct opened *o, const struct input_event *ev, int n)
{
    return evdev_write(&o->e, (const char __user *)ev, (uint32_t)n * (uint32_t)sizeof(*ev));
}

/** Fill `ev` with the event `type`, `code`, `value`, its time 0. */
static void event(struct input_event *ev, uint16_t type, uint16_t code, int32_t value)
{
    k_memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->code = code;
    ev->value = value;
}

/** A write injects its records as Linux's evdev_write: every open reads
 * them, the writer's among them, through the input core's filters and the
 * grab. */
static void write_rows(void)
{
    static struct input_event many[200];
    struct evdev *dev = mouse();
    struct input_event ev[4];
    struct opened a, b;
    struct wait_queue *wq;
    int i;

    open_dev(&a, 0);
    open_dev(&b, 0);
    CHECK("an open polls writable", evdev_poll(&a.e, POLLOUT, &wq) == POLLOUT);
    CHECK("a write of 0 bytes takes nothing", evdev_write(&a.e, (const char __user *)ev, 0) == 0);
    CHECK("a write of less than a record is -EINVAL",
          evdev_write(&a.e, (const char __user *)ev, sizeof(ev[0]) - 1) == -EINVAL);
    event(&ev[0], EV_REL, REL_X, 3);
    event(&ev[1], EV_SYN, SYN_REPORT, 0);
    CHECK("a write of a packet takes its bytes",
          evdev_write(&a.e, (const char __user *)ev, 2 * sizeof(ev[0]) + 5) ==
              2 * (int32_t)sizeof(ev[0]));
    CHECK("another open reads it",
          take(&b) > 0 && n_got == 2 && is(0, EV_REL, REL_X, 3) && is(1, EV_SYN, SYN_REPORT, 0));
    CHECK("so does the writer", take(&a) > 0 && n_got == 2 && is(0, EV_REL, REL_X, 3));
    event(&ev[0], EV_REL, REL_X, 0);
    event(&ev[1], EV_REL, REL_HWHEEL, 1);
    event(&ev[2], EV_SW, 0, 1);
    event(&ev[3], EV_SYN, SYN_DROPPED, 0);
    CHECK("records the filters drop are taken", put(&a, ev, 4) == 4 * (int32_t)sizeof(ev[0]));
    event(&ev[0], EV_SYN, SYN_REPORT, 0);
    put(&a, ev, 1);
    CHECK("and nobody reads them: no motion of 0, axis or switch it lacks, nor SYN_DROPPED",
          take(&b) == -EAGAIN);
    CHECK("a write from a bad pointer is -EFAULT",
          evdev_write(&a.e, (const char __user *)8, sizeof(ev[0])) == -EFAULT);
    for (i = 0; i < 200; i++) {
        event(&many[i], EV_SYN, SYN_REPORT, 0);
    }
    CHECK("a write takes at most 4096 bytes of whole records",
          put(&a, many, 200) == 4096 / (int32_t)sizeof(many[0]) * (int32_t)sizeof(many[0]));

    CHECK("the reader takes the grab", evdev_ioctl(&b.e, EVIOCGRAB, 1) == 0);
    event(&ev[0], EV_REL, REL_Y, -2);
    event(&ev[1], EV_SYN, SYN_REPORT, 0);
    put(&a, ev, 2);
    CHECK("the grab's holder alone reads a write",
          take(&a) == -EAGAIN && take(&b) > 0 && is(0, EV_REL, REL_Y, -2));
    CHECK("the revoked writer's write is -ENODEV",
          evdev_ioctl(&a.e, EVIOCREVOKE, 0) == 0 && put(&a, ev, 2) == -ENODEV);
    evdev_close(&a.e);
    evdev_close(&b.e);
    evdev_unregister(dev);
}

/** A keyboard's written keys, LEDs and repeat go on to input.c, a key only
 * while it passes the filters and no open holds the grab. */
static void write_keyboard_rows(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct input_event ev[2];
    struct opened a, b;
    struct evdev *kbd;

    k_memset(keys, 0, sizeof(keys));
    keys[KEY_A >> 3] |= (uint8_t)(1u << (KEY_A & 7));
    kbd = evdev_register("AT Translated Set 2 keyboard", EVDEV_BUS_I8042, 1, 1, keys);
    evdev_describe(kbd, "isa0060/serio0/input0", EVDEV_SCAN | EVDEV_KEYBOARD);
    open_dev(&a, 0);
    open_dev(&b, 0);
    n_written = 0;
    event(&ev[0], EV_KEY, KEY_A, 1);
    event(&ev[1], EV_SYN, SYN_REPORT, 0);
    put(&a, ev, 2);
    CHECK("a written key reaches the keyboard's opens", take(&b) > 0 && is(0, EV_KEY, KEY_A, 1));
    CHECK("and the console",
          n_written == 1 && written_type == EV_KEY && written_code == KEY_A && written_value == 1);
    put(&a, ev, 2);
    CHECK("a press of a key down is dropped, for the console too",
          take(&b) == -EAGAIN && n_written == 1);
    event(&ev[0], EV_LED, LED_CAPSL, 1);
    put(&a, ev, 1);
    CHECK("a written LED goes to input.c", n_written == 2 && written_type == EV_LED &&
                                               written_code == LED_CAPSL && written_value == 1);
    event(&ev[0], EV_REP, REP_DELAY, 400);
    put(&a, ev, 1);
    CHECK("so does a written repeat", n_written == 3 && written_type == EV_REP &&
                                          written_code == REP_DELAY && written_value == 400);
    evdev_ioctl(&b.e, EVIOCGRAB, 1);
    event(&ev[0], EV_KEY, KEY_A, 0);
    event(&ev[1], EV_SYN, SYN_REPORT, 0);
    put(&a, ev, 2);
    CHECK("a grabbed keyboard's written key reaches the holder alone",
          take(&b) > 0 && is(0, EV_KEY, KEY_A, 0) && n_written == 3);
    take(&a);
    evdev_close(&a.e);
    evdev_close(&b.e);
    evdev_unregister(kbd);
}

/** An i386 writer on the x86-64 kernel writes records of 16 bytes. */
static void write_compat_rows(void)
{
    struct evdev *dev = mouse();
    uint8_t rec[32];
    struct opened a, b;

    open_dev(&a, 0);
    open_dev(&b, 0);
    compat_caller = 1;
    k_memset(rec, 0, sizeof(rec));
    rec[8] = EV_REL; /* type, code and value at 8, 10 and 12 */
    rec[10] = REL_X;
    rec[12] = 7; /* the second record, all 0, is SYN_REPORT */
    CHECK("an i386 writer's record is 16 bytes",
          evdev_write(&a.e, (const char __user *)rec, 15) == -EINVAL &&
              evdev_write(&a.e, (const char __user *)rec, 32) == 32);
    compat_caller = 0;
    CHECK("and reads as its event",
          take(&b) > 0 && n_got == 2 && is(0, EV_REL, REL_X, 7) && is(1, EV_SYN, SYN_REPORT, 0));
    evdev_close(&a.e);
    evdev_close(&b.e);
    evdev_unregister(dev);
}

/** An i386 reader on the x86-64 kernel reads records of 16 bytes, with
 * 32-bit seconds and microseconds, as Linux's `input_event_compat`. */
static void compat_rows(void)
{
    struct evdev *dev = mouse();
    struct opened a;
    uint8_t buf[64];
    uint32_t u32;
    int32_t v;

    CHECK("an open for the i386 reader", open_dev(&a, 0) == 0);
    fixed = 1;
    fixed_real = 1700000000LL * 1000000000 + 123456789;
    packet(dev, EV_REL, REL_X, 5);
    compat_caller = 1;
    k_memset(buf, 0xEE, sizeof(buf));
    CHECK("a read of 15 bytes is -EINVAL", evdev_read(&a.e, (char __user *)buf, 15) == -EINVAL);
    CHECK("a read of 16 bytes takes one record", evdev_read(&a.e, (char __user *)buf, 16) == 16);
    k_memcpy(&u32, buf, 4);
    CHECK("its seconds in 32 bits", u32 == 1700000000);
    k_memcpy(&u32, buf + 4, 4);
    CHECK("its microseconds in 32 bits", u32 == 123456);
    k_memcpy(&v, buf + 12, 4);
    CHECK("its type, code and value at 8",
          buf[8] == EV_REL && buf[9] == 0 && buf[10] == REL_X && buf[11] == 0 && v == 5);
    CHECK("nothing past it", buf[16] == 0xEE);
    CHECK("the next read takes the SYN_REPORT",
          evdev_read(&a.e, (char __user *)buf, sizeof(buf)) == 16 && buf[8] == EV_SYN &&
              buf[16] == 0xEE);
    compat_caller = 0;
    packet(dev, EV_REL, REL_Y, 1);
    CHECK("an x86-64 reader still reads 24 bytes a record", take(&a) == 2 * 24 && n_got == 2);
    fixed = 0;
    evdev_close(&a.e);
    evdev_unregister(dev);
}

int main(void)
{
    motion_rows();
    revoke_rows();
    clock_rows();
    mask_rows();
    key_state_rows();
    abs_rows();
    compat_rows();
    write_rows();
    write_keyboard_rows();
    write_compat_rows();
    CHECK("every slot is free at the end", !devices[0].used && !devices[1].used);
    if (fails == 0) {
        printf("evdev-test: pass\n");
    }
    return fails ? 1 : 0;
}
