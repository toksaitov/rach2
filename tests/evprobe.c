/**
 * Reads the keyboards' event devices, /dev/input/eventN, and the mixer,
 * /dev/mixer, as Linux's programs read them.
 *
 * `evprobe ps2` and `evprobe usb` find the event device of the PS/2 or the
 * USB keyboard by its name, check what its ioctls answer, open it four
 * times and print "EVPROBE: press the keys". The suite then presses A,
 * Volume Up, Volume Down and Mute through the emulator's monitor, and each
 * of two readers must read the same eight packets: the key's MSC_SCAN, the
 * key's code pressed or released, and SYN_REPORT. The probe prints
 * "EVPROBE: flood", the suite presses twelve keys more, and the open that
 * read nothing all along must find its queue overflowed: a SYN_DROPPED
 * first. A nonblocking open of an empty queue, a read too small for a
 * record, poll, and EVIOCGRAB's two exclusions are checked on the way.
 *
 * `evprobe mixer HDA` and `evprobe mixer SB16` read and set the master
 * volume and PCM through OSS's ioctls, check the masks and
 * SOUND_MIXER_INFO, and require the level to stay set across a stream's
 * start and to follow the console's own AUDIO_SET_VOLUME.
 *
 * `evprobe button` finds the ACPI power button's event device, the FADT's
 * fixed button, prints "EVPROBE: press the button", and must read
 * KEY_POWER pressed and released once the suite presses it through the
 * emulator's monitor. `evprobe video` finds the ACPI video bus of the
 * suite's SSDT (tests/cases/machine/acpi_video.asl), calls its methods
 * through /proc/acpi/call, with integer and string arguments and a method
 * that is not there among them, and must read KEY_BRIGHTNESSDOWN and
 * KEY_BRIGHTNESSUP from the Notify of its \RTST.
 *
 * `evprobe mouse` and `evprobe usbmouse` find the PS/2 mouse's event device,
 * "ImExPS/2 Generic Explorer Mouse", or the USB one's, "USB HIDBP Mouse",
 * check its numbers, buttons, axes and properties as Linux gives them,
 * revoke one open, which then reads ENODEV, set another's clock to
 * CLOCK_MONOTONIC and print "EVPROBE: move the mouse". The suite then moves
 * the mouse by 10 and 5 through QMP, presses and releases the left button,
 * and turns the wheel up a detent and down one; the monotonic open and a
 * second, on the real time, must each read the motion, the button and both
 * turns, REL_WHEEL_HI_RES of 120 with each on USB, every record stamped
 * between the clock_gettime calls before and after on its own clock.
 *
 * `evprobe touchpad`, run by hand on the Lenovo, finds the Synaptics
 * touchpad's event device, prints its block as Linux's
 * /proc/bus/input/devices gives it and EVIOCGABS of each of its axes, to set
 * beside Debian's for the same pad, and requires the identity, bits and
 * coordinates Linux gives the Lenovo's pad in absolute mode.
 *
 * Prints "EVPROBE OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_NONBLOCK 04000

#define EAGAIN 11
#define ENODEV 19
#define EBUSY  16
#define EINVAL 22

#define POLLIN 1

/* Linux's evdev ioctls; a length goes in bits 16 to 29. */
#define EVIOCGVERSION   0x80044501L
#define EVIOCGID        0x80084502L
#define EVIOCGNAME(n)   (0x80004506L | (long)(n) << 16)
#define EVIOCGPHYS(n)   (0x80004507L | (long)(n) << 16)
#define EVIOCGKEY(n)    (0x80004518L | (long)(n) << 16)
#define EVIOCGBIT(t, n) ((0x80004520L + (t)) | (long)(n) << 16)
#define EVIOCGPROP(n)   (0x80004509L | (long)(n) << 16)
#define EVIOCGABS(a)    (0x80184540L + (a))
#define EVIOCGRAB       0x40044590L
#define EVIOCREVOKE     0x40044591L
#define EVIOCSCLOCKID   0x400445A0L

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define EV_SYN           0
#define EV_KEY           1
#define EV_REL           2
#define EV_MSC           4
#define REL_X            0
#define REL_Y            1
#define REL_HWHEEL       6
#define REL_WHEEL        8
#define REL_WHEEL_HI_RES 11
#define BTN_LEFT         0x110
#define BTN_EXTRA        0x114
#define SYN_REPORT       0
#define SYN_DROPPED      3
#define MSC_SCAN         4
#define KEY_A            30
#define BUS_USB          0x03
#define BUS_I8042        0x11
#define BUS_HOST         0x19

/* The keys of the ACPI devices, and Linux's product numbers of the power
 * button's and the video bus's event devices. */
#define KEY_POWER          116
#define KEY_BRIGHTNESSDOWN 224
#define KEY_BRIGHTNESSUP   225
#define PRODUCT_POWER      0x01
#define PRODUCT_VIDEO      0x06

/* OSS's mixer ioctls, and the console's audio requests
 * (rach2/sys/syscall.c). */
#define MIXER_READ_VOLUME     0x80044D00L
#define MIXER_WRITE_VOLUME    0xC0044D00L
#define MIXER_READ_BASS       0x80044D01L
#define MIXER_READ_PCM        0x80044D04L
#define MIXER_WRITE_PCM       0xC0044D04L
#define MIXER_READ_DEVMASK    0x80044DFEL
#define MIXER_READ_STEREODEVS 0x80044DFBL
#define MIXER_INFO            0x805C4D65L
#define AUDIO_START           0x4102L
#define AUDIO_STOP            0x4103L
#define AUDIO_SET_VOLUME      0x4105L

/* Linux's `struct input_event` as the C library lays it out: 16 bytes on
 * i686, 24 on x86-64. */
struct input_event {
    unsigned long sec;
    unsigned long usec;
    unsigned short type;
    unsigned short code;
    int value;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct mixer_info {
    char id[16];
    char name[32];
    int modify_counter;
    int fillers[10];
};

struct audio_start_params {
    unsigned int sample_rate;
    unsigned int format;
    unsigned int channels;
};

/* One packet as a key gives it: its MSC_SCAN, its code and its value. */
struct key_packet {
    long scan;
    int code;
    int value;
};

/* The four keys the suite presses first, each pressed and then released,
 * with the MSC_SCAN each keyboard gives: a translated set-1 code with 0x80
 * after E0 on the PS/2 keyboard, the keyboard page's usage on USB. */
static const struct {
    long ps2;
    long usb;
    int code;
} expected[4] = {{0x1E, 0x70004, KEY_A},
                 {0xB0, 0x70080, 115},  /* KEY_VOLUMEUP */
                 {0xAE, 0x70081, 114},  /* KEY_VOLUMEDOWN */
                 {0xA0, 0x7007F, 113}}; /* KEY_MUTE */

/* The keys the suite presses after "flood", which overflow a queue of 64
 * records at six records a key with the first four. */
#define FLOOD_KEYS 12

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/**
 * Set the `n` bytes at `p` to zero, before a system call fills them. The
 * stores are volatile, so that the compiler does not make them a call of
 * memset, which a probe without a C library lacks.
 */
static void clear(void *p, unsigned long n)
{
    volatile unsigned char *b = (volatile unsigned char *)p;

    while (n-- > 0) {
        *b++ = 0;
    }
}

/** Whether `s` begins with `prefix`. */
static int begins(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** Write `s` to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/** Write `v` in hexadecimal, with 0x before it. */
static void put_hex(long v)
{
    char digits[2 + 2 * sizeof(long) + 1];
    unsigned long u = (unsigned long)v;
    int i = (int)sizeof(digits) - 1;

    digits[i] = 0;
    do {
        digits[--i] = "0123456789abcdef"[u & 15];
        u >>= 4;
    } while (u != 0 && i > 2);
    digits[--i] = 'x';
    digits[--i] = '0';
    puts(digits + i);
}

/** Print "EVPROBE FAIL: ", `why` and, when `show` is nonzero, the value
 * `v`, and exit 1. */
static void fail_with(const char *why, int show, long v)
{
    puts("EVPROBE FAIL: ");
    puts(why);
    if (show) {
        puts(": ");
        put_hex(v);
    }
    puts("\n");
    sys_exit(1);
}

/** Print "EVPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    fail_with(why, 0, 0);
}

/** ioctl(2) of `fd`. */
static long ioctl(int fd, long request, long arg)
{
    return syscall3(SYS_IOCTL, fd, request, arg);
}

/** Open `path` with `flags`, failing the probe with `why` when it does not
 * open. */
static int open_or_fail(const char *path, int flags, const char *why)
{
    long fd = syscall3(SYS_OPEN, (long)path, flags, 0);

    if (fd < 0) {
        fail_with(why, 1, -fd);
    }
    return (int)fd;
}

/** The path of event device `n`, in a buffer of the probe's. */
static const char *event_path(int n)
{
    static char path[24] = "/dev/input/event";
    int at = 16;

    if (n >= 10) {
        path[at++] = (char)('0' + n / 10);
    }
    path[at++] = (char)('0' + n % 10);
    path[at] = 0;
    return path;
}

/** Find the event device whose name begins with `name` and give its path;
 * fail the probe when there is none. */
static const char *find_device(const char *name)
{
    char text[64];
    int n;

    for (n = 0; n < 32; n++) {
        long fd = syscall3(SYS_OPEN, (long)event_path(n), O_RDONLY, 0);
        long len;

        if (fd < 0) {
            continue;
        }
        len = ioctl((int)fd, EVIOCGNAME(sizeof(text) - 1), (long)text);
        syscall3(SYS_CLOSE, fd, 0, 0);
        if (len > 0) {
            text[len] = 0;
            if (begins(text, name)) {
                puts("EVPROBE: ");
                puts(event_path(n));
                puts(" is ");
                puts(text);
                puts("\n");
                return event_path(n);
            }
        }
    }
    fail("no event device has the name");
    return 0;
}

/** Whether bit `n` of the set `bits` is on. */
static int bit_on(const unsigned char *bits, int n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

/** Check EVIOCGVERSION, EVIOCGID and EVIOCGPHYS of the open device `fd`,
 * of the bus `bus`. */
static void check_ids(int fd, int bus)
{
    unsigned short id[4];
    char phys[64];
    long version = 0;
    long len;

    clear(id, sizeof(id));
    if (ioctl(fd, EVIOCGVERSION, (long)&version) != 0 || (int)version != 0x010001) {
        fail_with("EVIOCGVERSION is not 0x010001", 1, version);
    }
    if (ioctl(fd, EVIOCGID, (long)id) != 0 || id[0] != bus) {
        fail_with("EVIOCGID gives another bus", 1, id[0]);
    }
    len = ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), (long)phys);
    if (len <= 0) {
        fail_with("EVIOCGPHYS gives nothing", 1, -len);
    }
    phys[len] = 0;
    if (!begins(phys, bus == BUS_USB ? "usb-" : "isa0060/serio0/input0")) {
        fail("EVIOCGPHYS gives another place");
    }
}

/** Check EVIOCGBIT and EVIOCGKEY of the open device `fd`. */
static void check_bits(int fd)
{
    unsigned char bits[96];
    int i;

    for (i = 0; i < 4; i++) {
        bits[i] = 0xFF;
    }
    if (ioctl(fd, EVIOCGBIT(0, 4), (long)bits) <= 0 ||
        bits[0] != (1 << EV_SYN | 1 << EV_KEY | 1 << EV_MSC)) {
        fail_with("EVIOCGBIT(0) is not EV_SYN, EV_KEY and EV_MSC", 1, bits[0]);
    }
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), (long)bits) != (long)sizeof(bits)) {
        fail("EVIOCGBIT(EV_KEY) is not 96 bytes");
    }
    for (i = 0; i < 4; i++) {
        if (!bit_on(bits, expected[i].code)) {
            fail_with("EVIOCGBIT(EV_KEY) lacks a key", 1, expected[i].code);
        }
    }
    if (ioctl(fd, EVIOCGBIT(EV_MSC, 1), (long)bits) != 1 || !bit_on(bits, MSC_SCAN)) {
        fail("EVIOCGBIT(EV_MSC) lacks MSC_SCAN");
    }
    if (ioctl(fd, EVIOCGKEY(sizeof(bits)), (long)bits) != (long)sizeof(bits)) {
        fail("EVIOCGKEY is not 96 bytes");
    }
    for (i = 0; i < (int)sizeof(bits); i++) {
        if (bits[i] != 0) {
            fail("EVIOCGKEY has a key down before any was pressed");
        }
    }
}

/**
 * Read the packets of keys from `fd` into `out`, up to `max`, until
 * `releases` keys have been released; a repeat, value 2, is passed over.
 * A packet must be its MSC_SCAN, then its key or none, then SYN_REPORT.
 * Gives the packets read.
 */
static int read_keys(int fd, struct key_packet *out, int max, int releases)
{
    struct input_event ev[16];
    struct key_packet p;
    int n = 0, seen = 0;

    clear(ev, sizeof(ev));
    p.scan = -1;
    p.code = -1;
    p.value = -1;
    while (seen < releases) {
        long got = syscall3(SYS_READ, fd, (long)ev, (long)sizeof(ev));
        int i;

        if (got <= 0 || got % (long)sizeof(ev[0]) != 0) {
            fail_with("a read of the event device did not give whole records", 1, got);
        }
        for (i = 0; i < (int)(got / (long)sizeof(ev[0])); i++) {
            if (ev[i].type == EV_MSC && ev[i].code == MSC_SCAN) {
                p.scan = ev[i].value;
            } else if (ev[i].type == EV_KEY) {
                p.code = ev[i].code;
                p.value = ev[i].value;
            } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                if (p.scan < 0) {
                    fail("a packet came without its MSC_SCAN");
                }
                if (p.value != 2 && n < max) {
                    out[n++] = p;
                }
                if (p.value == 0) {
                    seen++;
                }
                p.scan = -1;
                p.code = -1;
                p.value = -1;
            } else {
                fail_with("a record of another type came", 1, ev[i].type);
            }
        }
    }
    return n;
}

/** Check that `got`, `n` packets, are the four keys pressed and released,
 * with the scans of a USB keyboard when `usb`; `who` names the reader. */
static void check_keys(const struct key_packet *got, int n, int usb, const char *who)
{
    int i;

    if (n != 8) {
        puts(who);
        fail_with(" did not read eight packets", 1, n);
    }
    for (i = 0; i < 8; i++) {
        long scan = usb ? expected[i / 2].usb : expected[i / 2].ps2;

        if (got[i].scan != scan) {
            puts(who);
            fail_with(" read another MSC_SCAN", 1, got[i].scan);
        }
        if (got[i].code != expected[i / 2].code) {
            puts(who);
            fail_with(" read another key", 1, got[i].code);
        }
        if (got[i].value != !(i & 1)) {
            puts(who);
            fail_with(" read a key pressed or released out of turn", 1, got[i].value);
        }
    }
}

/**
 * Check what the opens `a`, `b` and the nonblocking `idle` of one device
 * refuse before any key: a nonblocking read of an empty queue, a read
 * smaller than a record, a second grab and the give-back of a grab not
 * held; and that poll finds nothing to read.
 */
static void check_refusals(int a, int b, int idle)
{
    struct input_event ev;
    struct pollfd pfd;
    long rc;

    if ((rc = syscall3(SYS_READ, idle, (long)&ev, sizeof(ev))) != -EAGAIN) {
        fail_with("a nonblocking read of an empty queue is not EAGAIN", 1, rc);
    }
    if ((rc = syscall3(SYS_READ, a, (long)&ev, sizeof(ev) - 1)) != -EINVAL) {
        fail_with("a read smaller than a record is not EINVAL", 1, rc);
    }
    if (ioctl(a, EVIOCGRAB, 1) != 0) {
        fail("EVIOCGRAB did not take the device");
    }
    if ((rc = ioctl(b, EVIOCGRAB, 1)) != -EBUSY) {
        fail_with("a second EVIOCGRAB is not EBUSY", 1, rc);
    }
    if (ioctl(a, EVIOCGRAB, 0) != 0) {
        fail("EVIOCGRAB did not give the device back");
    }
    if ((rc = ioctl(b, EVIOCGRAB, 0)) != -EINVAL) {
        fail_with("giving back a grab not held is not EINVAL", 1, rc);
    }
    pfd.fd = a;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if ((rc = syscall3(SYS_POLL, (long)&pfd, 1, 0)) != 0) {
        fail_with("poll finds a record before any key", 1, rc);
    }
}

/** The run of `evprobe ps2` or `evprobe usb`. */
static void keyboard(int usb)
{
    const char *path = find_device(usb ? "USB keyboard" : "AT Translated Set 2 keyboard");
    struct key_packet first[16], second[16], flood[2 * FLOOD_KEYS + 4];
    struct input_event ev;
    struct pollfd pfd;
    int a, b, lag, idle;
    long rc;

    clear(first, sizeof(first));
    clear(second, sizeof(second));
    clear(&ev, sizeof(ev));
    a = open_or_fail(path, O_RDONLY, "the event device did not open");
    b = open_or_fail(path, O_RDONLY, "the event device did not open twice");
    lag = open_or_fail(path, O_RDONLY | O_NONBLOCK, "the event device did not open thrice");
    idle = open_or_fail(path, O_RDONLY | O_NONBLOCK, "the event device did not open four times");
    check_ids(a, usb ? BUS_USB : BUS_I8042);
    check_bits(a);
    check_refusals(a, b, idle);

    puts("EVPROBE: press the keys\n");
    pfd.fd = a;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (syscall3(SYS_POLL, (long)&pfd, 1, -1) != 1 || !(pfd.revents & POLLIN)) {
        fail("poll did not wake for the keys");
    }
    check_keys(first, read_keys(a, first, 16, 4), usb, "the first reader");
    check_keys(second, read_keys(b, second, 16, 4), usb, "the second reader");
    puts("EVPROBE: keys: read by both readers\n");

    puts("EVPROBE: flood\n");
    read_keys(a, flood, 2 * FLOOD_KEYS + 4, FLOOD_KEYS);
    rc = syscall3(SYS_READ, lag, (long)&ev, sizeof(ev));
    if (rc != (long)sizeof(ev) || ev.type != EV_SYN || ev.code != SYN_DROPPED) {
        fail_with("the reader that fell behind did not read SYN_DROPPED first", 1, ev.code);
    }
    puts("EVPROBE: flood: SYN_DROPPED\n");
    syscall3(SYS_CLOSE, a, 0, 0);
    syscall3(SYS_CLOSE, b, 0, 0);
    syscall3(SYS_CLOSE, lag, 0, 0);
    syscall3(SYS_CLOSE, idle, 0, 0);
}

/** The time of clock `clock` now, in microseconds. */
static long long now_usec(int clock)
{
    struct timespec ts;

    clear(&ts, sizeof(ts));
    if (syscall2(SYS_CLOCK_GETTIME, clock, (long)&ts) != 0) {
        fail("clock_gettime failed");
    }
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/** Check the identity and capabilities of the mouse open on `fd`: its bus,
 * vendor and product, place, types, buttons, axes and properties, with
 * REL_HWHEEL and INPUT_PROP_POINTER on PS/2 and REL_WHEEL_HI_RES on USB. */
static void check_mouse(int fd, int usb)
{
    unsigned char bits[96];
    unsigned short id[4];
    unsigned char abs[24];
    char phys[64];
    long len;
    int i;

    clear(id, sizeof(id));
    if (ioctl(fd, EVIOCGID, (long)id) != 0 || id[0] != (usb ? BUS_USB : BUS_I8042)) {
        fail_with("the mouse's EVIOCGID gives another bus", 1, id[0]);
    }
    if (!usb && (id[1] != 2 || id[2] != 6)) {
        fail_with("the PS/2 mouse is not vendor 2, product PSMOUSE_IMEX", 1,
                  (long)id[1] << 16 | id[2]);
    }
    len = ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), (long)phys);
    if (len <= 0) {
        fail_with("the mouse's EVIOCGPHYS gives nothing", 1, -len);
    }
    phys[len] = 0;
    if (!begins(phys, usb ? "usb-" : "isa0060/serio1/input0")) {
        fail("the mouse's EVIOCGPHYS gives another place");
    }
    clear(bits, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(0, 4), (long)bits) <= 0 ||
        bits[0] != (1 << EV_SYN | 1 << EV_KEY | 1 << EV_REL)) {
        fail_with("the mouse's EVIOCGBIT(0) is not EV_SYN, EV_KEY and EV_REL", 1, bits[0]);
    }
    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_REL, 2), (long)bits);
    if (!bit_on(bits, REL_X) || !bit_on(bits, REL_Y) || !bit_on(bits, REL_WHEEL) ||
        bit_on(bits, REL_HWHEEL) == usb || bit_on(bits, REL_WHEEL_HI_RES) != usb) {
        fail_with("the mouse's axes are not Linux's", 1, bits[1] << 8 | bits[0]);
    }
    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), (long)bits);
    for (i = BTN_LEFT; i <= BTN_EXTRA; i++) {
        if (!bit_on(bits, i)) {
            fail_with("the mouse lacks a button", 1, i);
        }
    }
    clear(bits, sizeof(bits));
    if (ioctl(fd, EVIOCGPROP(4), (long)bits) != 4 || bits[0] != (usb ? 0 : 1)) {
        fail_with("the mouse's EVIOCGPROP is not Linux's", 1, bits[0]);
    }
    if ((len = ioctl(fd, EVIOCGABS(0), (long)abs)) != -EINVAL) {
        fail_with("EVIOCGABS of a mouse is not EINVAL", 1, len);
    }
}

/** What a reader of the mouse has seen, a bit each. */
#define SEEN_X     0x01
#define SEEN_Y     0x02
#define SEEN_DOWN  0x04
#define SEEN_UP    0x08
#define SEEN_WHEEL 0x10 /* a turn up */
#define SEEN_BACK  0x20 /* a turn down */
#define SEEN_ALL   0x3F

/* What a reader of the mouse has read so far. */
struct mouse_read {
    int seen;  /* SEEN_* */
    int hires; /* REL_WHEEL_HI_RES records of a detent */
    int dx;    /* the motion summed */
    int dy;
};

/** Take the record `ev` into `r`; fail the probe, naming the reader `who`,
 * for a record a mouse does not send. */
static void take_record(const struct input_event *ev, struct mouse_read *r, const char *who)
{
    if (ev->type == EV_REL && ev->code == REL_X) {
        r->dx += ev->value;
        r->seen |= SEEN_X;
    } else if (ev->type == EV_REL && ev->code == REL_Y) {
        r->dy += ev->value;
        r->seen |= SEEN_Y;
    } else if (ev->type == EV_KEY && ev->code == BTN_LEFT) {
        r->seen |= ev->value ? SEEN_DOWN : SEEN_UP;
    } else if (ev->type == EV_REL && ev->code == REL_WHEEL) {
        r->seen |= ev->value == 1 ? SEEN_WHEEL : ev->value == -1 ? SEEN_BACK : 0;
    } else if (ev->type == EV_REL && ev->code == REL_WHEEL_HI_RES) {
        r->hires += ev->value == 120 || ev->value == -120;
    } else if (ev->type != EV_SYN || ev->code != SYN_REPORT) {
        puts(who);
        fail_with(" read a record of another kind", 1, ev->type << 16 | ev->code);
    }
}

/**
 * Read the mouse on `fd` until the motion of 10 and 5, the left button
 * pressed and released and the wheel turned up and down have all come, with
 * REL_WHEEL_HI_RES of 120 beside each turn when `usb`; each record must be
 * stamped between `from` and the time of `clock` after the read. `who`
 * names the reader.
 */
static void read_mouse(int fd, int usb, int clock, long long from, const char *who)
{
    struct input_event ev[16];
    struct mouse_read r;

    clear(ev, sizeof(ev));
    clear(&r, sizeof(r));
    while (r.seen != SEEN_ALL || r.dx < 10 || r.dy < 5) {
        long got = syscall3(SYS_READ, fd, (long)ev, (long)sizeof(ev));
        long long to = now_usec(clock);
        int i;

        if (got <= 0 || got % (long)sizeof(ev[0]) != 0) {
            puts(who);
            fail_with(" did not read whole records", 1, got);
        }
        for (i = 0; i < (int)(got / (long)sizeof(ev[0])); i++) {
            long long at = (long long)ev[i].sec * 1000000 + (long long)ev[i].usec;

            if (at < from || at > to) {
                puts(who);
                fail_with(" read a record stamped outside the clock_gettime calls", 1,
                          (long)(at - from));
            }
            take_record(&ev[i], &r, who);
        }
    }
    if (r.dx != 10 || r.dy != 5) {
        puts(who);
        fail_with(" read another motion", 1, (long)r.dx << 16 | (r.dy & 0xFFFF));
    }
    if (usb && r.hires != 2) {
        puts(who);
        fail_with(" did not read REL_WHEEL_HI_RES with each turn", 1, r.hires);
    }
}

/** The run of `evprobe mouse` or `evprobe usbmouse`. */
static void mouse(int usb)
{
    const char *path = find_device(usb ? "USB HIDBP Mouse" : "ImExPS/2 Generic Explorer Mouse");
    long long mono, real;
    struct input_event ev;
    int a, b, gone, clock;
    long rc;

    a = open_or_fail(path, O_RDONLY, "the mouse did not open");
    b = open_or_fail(path, O_RDONLY, "the mouse did not open twice");
    gone = open_or_fail(path, O_RDONLY, "the mouse did not open thrice");
    check_mouse(a, usb);
    clock = 5;
    if ((rc = ioctl(a, EVIOCSCLOCKID, (long)&clock)) != -EINVAL) {
        fail_with("EVIOCSCLOCKID of an unknown clock is not EINVAL", 1, rc);
    }
    clock = CLOCK_MONOTONIC;
    if (ioctl(a, EVIOCSCLOCKID, (long)&clock) != 0) {
        fail("EVIOCSCLOCKID did not take CLOCK_MONOTONIC");
    }
    if ((rc = ioctl(gone, EVIOCREVOKE, 1)) != -EINVAL) {
        fail_with("EVIOCREVOKE with an argument is not EINVAL", 1, rc);
    }
    if (ioctl(gone, EVIOCREVOKE, 0) != 0) {
        fail("EVIOCREVOKE failed");
    }
    if ((rc = syscall3(SYS_READ, gone, (long)&ev, sizeof(ev))) != -ENODEV) {
        fail_with("a revoked open does not read ENODEV", 1, rc);
    }
    if ((rc = ioctl(gone, EVIOCGRAB, 1)) != -ENODEV) {
        fail_with("a revoked open's ioctl is not ENODEV", 1, rc);
    }
    mono = now_usec(CLOCK_MONOTONIC);
    real = now_usec(CLOCK_REALTIME);
    puts("EVPROBE: move the mouse\n");
    read_mouse(a, usb, CLOCK_MONOTONIC, mono, "the monotonic reader");
    read_mouse(b, usb, CLOCK_REALTIME, real, "the real-time reader");
    puts("EVPROBE: mouse: read by both readers\n");
    syscall3(SYS_CLOSE, a, 0, 0);
    syscall3(SYS_CLOSE, b, 0, 0);
    syscall3(SYS_CLOSE, gone, 0, 0);
}

/** Read the mixer's volume through `fd`, failing the probe when it cannot. */
static int volume(int fd)
{
    int v = -1;

    if (ioctl(fd, MIXER_READ_VOLUME, (long)&v) != 0) {
        fail("SOUND_MIXER_READ_VOLUME failed");
    }
    return v;
}

/** Check that the volume of `fd` reads 100 at boot, and that writes set it
 * and give the level set back, the louder side of two. */
static void check_levels(int fd)
{
    int v;

    if ((v = volume(fd)) != 0x6464) {
        fail_with("the volume at boot is not 100 on both sides", 1, v);
    }
    v = 0x3232;
    if (ioctl(fd, MIXER_WRITE_VOLUME, (long)&v) != 0 || v != 0x3232) {
        fail_with("SOUND_MIXER_WRITE_VOLUME of 50 did not give 50 back", 1, v);
    }
    if ((v = volume(fd)) != 0x3232) {
        fail_with("the volume set to 50 does not read 50", 1, v);
    }
    v = 0x2050;
    if (ioctl(fd, MIXER_WRITE_VOLUME, (long)&v) != 0 || v != 0x5050) {
        fail_with("a write of 80 and 32 did not set the louder, 80", 1, v);
    }
}

/** Check the masks of `fd`, which have the volume and PCM, and the SB16's
 * nothing else; that PCM is the level of the caller's own playback, 100 and
 * fixed while it has none and its own level beside the volume once it opens
 * /dev/dsp; a channel it lacks; and that SOUND_MIXER_INFO names the driver
 * `id` and counts the writes. */
static void check_info(int fd, const char *id)
{
    struct mixer_info info;
    int v, dsp;
    long rc;

    if (ioctl(fd, MIXER_READ_DEVMASK, (long)&v) != 0 || (v & 0x11) != 0x11 ||
        (begins(id, "SB16") && v != 0x11)) {
        fail_with("SOUND_MIXER_READ_DEVMASK lacks the volume and PCM", 1, v);
    }
    if (ioctl(fd, MIXER_READ_STEREODEVS, (long)&v) != 0 || v != 0) {
        fail_with("SOUND_MIXER_READ_STEREODEVS is not none", 1, v);
    }
    if (ioctl(fd, MIXER_READ_PCM, (long)&v) != 0 || v != 0x6464) {
        fail_with("PCM at boot is not 100 on both sides", 1, v);
    }
    v = 0x3232;
    if (ioctl(fd, MIXER_WRITE_PCM, (long)&v) != 0 || v != 0x6464 || volume(fd) != 0x5050) {
        fail_with("PCM of a process that plays nothing did not stay 100", 1, v);
    }
    dsp = open_or_fail("/dev/dsp", O_WRONLY, "/dev/dsp did not open");
    v = 0x3232;
    if (ioctl(fd, MIXER_WRITE_PCM, (long)&v) != 0 || v != 0x3232 || volume(fd) != 0x5050) {
        fail_with("PCM set to 50 moves the volume", 1, v);
    }
    if (ioctl(fd, MIXER_READ_PCM, (long)&v) != 0 || v != 0x3232) {
        fail_with("PCM set to 50 does not read 50", 1, v);
    }
    v = 0x6464;
    if (ioctl(fd, MIXER_WRITE_PCM, (long)&v) != 0 || v != 0x6464) {
        fail_with("PCM set back to 100 did not give 100 back", 1, v);
    }
    syscall3(SYS_CLOSE, dsp, 0, 0);
    if ((rc = ioctl(fd, MIXER_READ_BASS, (long)&v)) != -EINVAL) {
        fail_with("a channel the mixer lacks is not EINVAL", 1, rc);
    }
    if (ioctl(fd, MIXER_INFO, (long)&info) != 0) {
        fail("SOUND_MIXER_INFO failed");
    }
    info.id[sizeof(info.id) - 1] = 0;
    info.name[sizeof(info.name) - 1] = 0;
    if (!begins(info.id, id) || info.id[strlen(id)] != 0 || info.name[0] == 0 ||
        info.modify_counter < 2) {
        fail("SOUND_MIXER_INFO gives another driver or no writes");
    }
    puts("EVPROBE: mixer: ");
    puts(info.id);
    puts(", ");
    puts(info.name);
    puts("\n");
}

/** The run of `evprobe mixer ID`, `id` the driver's SOUND_MIXER_INFO id:
 * the levels, the masks and the info, then the level across a stream's
 * start and after the console's own request. */
static void mixer(const char *id)
{
    struct audio_start_params start;
    int fd = open_or_fail("/dev/mixer", O_RDWR, "/dev/mixer did not open");
    int console = open_or_fail("/dev/console", O_WRONLY, "/dev/console did not open");
    int v;
    long rc;

    check_levels(fd);
    check_info(fd, id);
    start.sample_rate = 22050;
    start.format = 1;
    start.channels = 2;
    if ((rc = ioctl(console, AUDIO_START, (long)&start)) != 0) {
        fail_with("AUDIO_START failed", 1, -rc);
    }
    ioctl(console, AUDIO_STOP, 0);
    if ((v = volume(fd)) != 0x5050) {
        fail_with("the volume did not stay at 80 across a stream's start", 1, v);
    }
    if (ioctl(console, AUDIO_SET_VOLUME, 70) != 0 || (v = volume(fd)) != 0x4646) {
        fail_with("the console's AUDIO_SET_VOLUME of 70 does not read 70", 1, v);
    }
    v = 0x6464;
    ioctl(fd, MIXER_WRITE_VOLUME, (long)&v);
    syscall3(SYS_CLOSE, console, 0, 0);
    syscall3(SYS_CLOSE, fd, 0, 0);
}

/**
 * Check what the ioctls of `fd`, an ACPI device's event device, answer:
 * the host's bus, Linux's product number `product`, the place `phys`, and
 * keys alone, with `key` among them.
 */
static void check_acpi_device(int fd, int product, const char *phys, int key)
{
    unsigned short id[4];
    unsigned char bits[96];
    char text[64];
    long len;

    clear(id, sizeof(id));
    if (ioctl(fd, EVIOCGID, (long)id) != 0 || id[0] != BUS_HOST || id[2] != product) {
        fail_with("EVIOCGID gives another bus or product", 1, (long)id[0] << 16 | id[2]);
    }
    len = ioctl(fd, EVIOCGPHYS(sizeof(text) - 1), (long)text);
    if (len <= 0) {
        fail_with("EVIOCGPHYS gives nothing", 1, -len);
    }
    text[len] = 0;
    if (!begins(text, phys)) {
        fail("EVIOCGPHYS gives another place");
    }
    bits[0] = 0xFF;
    if (ioctl(fd, EVIOCGBIT(0, 4), (long)bits) <= 0 || bits[0] != (1 << EV_SYN | 1 << EV_KEY)) {
        fail_with("EVIOCGBIT(0) is not EV_SYN and EV_KEY", 1, bits[0]);
    }
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), (long)bits) != (long)sizeof(bits) ||
        !bit_on(bits, key)) {
        fail_with("EVIOCGBIT(EV_KEY) lacks a key", 1, key);
    }
}

/**
 * Read `n` keys from `fd`, each as Linux's ACPI drivers report one: the key
 * pressed, SYN_REPORT, the key released, SYN_REPORT, with no MSC_SCAN; the
 * codes go in `codes`.
 */
static void read_acpi_keys(int fd, int *codes, int n)
{
    struct input_event ev;
    int k, step;

    for (k = 0; k < n; k++) {
        for (step = 0; step < 4; step++) {
            long got = syscall3(SYS_READ, fd, (long)&ev, (long)sizeof(ev));

            if (got != (long)sizeof(ev)) {
                fail_with("a read of the event device did not give a record", 1, got);
            }
            if (step % 2 == 1) {
                if (ev.type != EV_SYN || ev.code != SYN_REPORT) {
                    fail_with("a key's record came without its SYN_REPORT", 1, ev.type);
                }
            } else if (ev.type != EV_KEY || ev.value != (step == 0) ||
                       (step == 2 && ev.code != codes[k])) {
                fail_with("a record came that is not a key pressed and released", 1,
                          (long)ev.type << 16 | ev.code);
            } else {
                codes[k] = ev.code;
            }
        }
    }
}

/* The touchpad's axes and keys, and EV_ABS. */
#define EV_ABS             3
#define ABS_X              0x00
#define ABS_Y              0x01
#define ABS_PRESSURE       0x18
#define ABS_TOOL_WIDTH     0x1c
#define ABS_MT_SLOT        0x2f
#define ABS_MT_POSITION_X  0x35
#define ABS_MT_POSITION_Y  0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MT_PRESSURE    0x3a

/* Linux's `struct input_absinfo`. */
struct input_absinfo {
    int value;
    int minimum;
    int maximum;
    int fuzz;
    int flat;
    int resolution;
};

/** Write `v` in decimal. */
static void put_dec(long v)
{
    char digits[24];
    unsigned long u = v < 0 ? 0 - (unsigned long)v : (unsigned long)v;
    int i = (int)sizeof(digits) - 1;

    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0) {
        digits[--i] = '-';
    }
    puts(digits + i);
}

/** Write `v` into `out` in hex, at least `width` digits, with no 0x and no
 * terminator; return the digits written. */
static int hex_into(char *out, unsigned long v, int width)
{
    char digits[8];
    int n = 0, i;

    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0 && n < 8);
    while (n < width) {
        digits[n++] = '0';
    }
    for (i = 0; i < n; i++) {
        out[i] = digits[n - 1 - i];
    }
    return n;
}

/** Write `v` in hex, `width` digits at least, with no 0x. */
static void put_hex_width(unsigned long v, int width)
{
    char text[9];

    text[hex_into(text, v, width)] = 0;
    puts(text);
}

/** The 32-bit word at `b`, little-endian. */
static unsigned long word32(const unsigned char *b)
{
    return (unsigned long)b[3] << 24 | (unsigned long)b[2] << 16 | (unsigned long)b[1] << 8 | b[0];
}

/**
 * Write the set of `n` bytes at `bits` as /proc/bus/input/devices prints the
 * line `label` on x86-64: its 64-bit words from the highest that is not 0,
 * in hex, spaced, or a single 0. The probe prints it so on i686 too, since
 * the reference is Debian's x86-64 kernel, and takes each word as two
 * halves, so that it needs no 64-bit arithmetic. Returns whether the text
 * is `want`.
 */
static int put_set(const char *label, const unsigned char *bits, int n, const char *want)
{
    char text[16 * 17 + 2];
    int at = 0, w, i, started = 0;

    for (w = n / 8 - 1; w >= 0; w--) {
        unsigned long lo = word32(bits + w * 8), hi = word32(bits + w * 8 + 4);

        if (!started && !hi && !lo) {
            continue;
        }
        if (started) {
            text[at++] = ' ';
        }
        started = 1;
        if (hi) {
            at += hex_into(text + at, hi, 0);
            at += hex_into(text + at, lo, 8);
        } else {
            at += hex_into(text + at, lo, 0);
        }
    }
    if (!started) {
        text[at++] = '0';
    }
    text[at] = 0;
    puts("B: ");
    puts(label);
    puts("=");
    puts(text);
    puts("\n");
    for (i = 0; want[i] && want[i] == text[i]; i++) {
    }
    return want[i] == text[i];
}

/** Print the touchpad's EVIOCGABS of `code` on `fd`; return whether its
 * limits are `min` and `max`, or any when `max` is below `min`. */
static int put_abs(int fd, int code, int min, int max)
{
    struct input_absinfo a;

    clear(&a, sizeof(a));
    if (ioctl(fd, EVIOCGABS(code), (long)&a) != 0) {
        fail_with("EVIOCGABS of a set axis failed", 1, code);
    }
    puts("EVPROBE: abs ");
    put_hex(code);
    puts(" value min max fuzz flat res ");
    put_dec(a.value);
    puts(" ");
    put_dec(a.minimum);
    puts(" ");
    put_dec(a.maximum);
    puts(" ");
    put_dec(a.fuzz);
    puts(" ");
    put_dec(a.flat);
    puts(" ");
    put_dec(a.resolution);
    puts("\n");
    return max < min || (a.minimum == min && a.maximum == max);
}

/**
 * The run of `evprobe touchpad`: find the Synaptics touchpad's event device,
 * print its block as /proc/bus/input/devices gives it and EVIOCGABS of each
 * axis it has, for comparison with Debian's on the same machine, and fail
 * unless they are what Linux gives the Lenovo's pad, model 1 with firmware
 * 8.1, capabilities 0xd00123/0x840300/0x2a800, in absolute mode: the bits
 * of tests/host/synaptics_hosttest.c and the coordinates Debian's psmouse
 * logged (dev-log/70-wifi-chip/linux/dmesg.txt). The resolution is printed
 * and not judged, since that log does not give it.
 */
static void touchpad(void)
{
    const char *path = find_device("SynPS/2 Synaptics TouchPad");
    unsigned char bits[96];
    unsigned short id[4];
    char text[64];
    long len;
    int fd, ok = 1, a;

    fd = open_or_fail(path, O_RDONLY, "the touchpad did not open");
    clear(id, sizeof(id));
    if (ioctl(fd, EVIOCGID, (long)id) != 0) {
        fail("the touchpad's EVIOCGID failed");
    }
    puts("I: Bus=");
    put_hex_width(id[0], 4);
    puts(" Vendor=");
    put_hex_width(id[1], 4);
    puts(" Product=");
    put_hex_width(id[2], 4);
    puts(" Version=");
    put_hex_width(id[3], 4);
    puts("\n");
    ok &= id[0] == BUS_I8042 && id[1] == 2 && id[2] == 7 && id[3] == 0x01b1;
    len = ioctl(fd, EVIOCGNAME(sizeof(text) - 1), (long)text);
    text[len > 0 ? len : 0] = 0;
    puts("N: Name=\"");
    puts(text);
    puts("\"\n");
    len = ioctl(fd, EVIOCGPHYS(sizeof(text) - 1), (long)text);
    text[len > 0 ? len : 0] = 0;
    puts("P: Phys=");
    puts(text);
    puts("\n");
    ok &= begins(text, "isa0060/serio1/input0") && text[21] == 0;

    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGPROP(8), (long)bits);
    ok &= put_set("PROP", bits, 8, "1");
    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGBIT(0, 8), (long)bits);
    ok &= put_set("EV", bits, 8, "b");
    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), (long)bits);
    ok &= put_set("KEY", bits, 96, "e520 30000 0 0 0 0");
    clear(bits, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_ABS, 8), (long)bits);
    ok &= put_set("ABS", bits, 8, "660800011000003");

    for (a = 0; a < 64; a++) {
        if (!bit_on(bits, a)) {
            continue;
        }
        if (a == ABS_X || a == ABS_MT_POSITION_X) {
            ok &= put_abs(fd, a, 1314, 5670);
        } else if (a == ABS_Y || a == ABS_MT_POSITION_Y) {
            ok &= put_abs(fd, a, 1098, 4794);
        } else if (a == ABS_PRESSURE || a == ABS_MT_PRESSURE) {
            ok &= put_abs(fd, a, 0, 255);
        } else if (a == ABS_TOOL_WIDTH) {
            ok &= put_abs(fd, a, 0, 15);
        } else if (a == ABS_MT_SLOT) {
            ok &= put_abs(fd, a, 0, 1);
        } else if (a == ABS_MT_TRACKING_ID) {
            ok &= put_abs(fd, a, 0, 65535);
        } else {
            ok &= put_abs(fd, a, 1, 0);
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (!ok) {
        fail("the touchpad's capabilities are not what Linux gives it");
    }
}

/** The run of `evprobe button`. */
static void button(void)
{
    int fd = open_or_fail(find_device("Power Button"), O_RDONLY, "the button did not open");
    int code = -1;

    check_acpi_device(fd, PRODUCT_POWER, "LNXPWRBN/button/input0", KEY_POWER);
    puts("EVPROBE: press the button\n");
    read_acpi_keys(fd, &code, 1);
    if (code != KEY_POWER) {
        fail_with("the power button gave another key", 1, code);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
}

/** Whether the NUL-terminated `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Write `request` to /proc/acpi/call, which must take it whole, and read the
 * result back from a second open: `want` and its NUL.
 */
static void call(const char *request, const char *want)
{
    char got[128];
    int fd = open_or_fail("/proc/acpi/call", O_WRONLY, "/proc/acpi/call did not open to write");
    long n = syscall3(SYS_WRITE, fd, (long)request, (long)strlen(request));

    syscall3(SYS_CLOSE, fd, 0, 0);
    clear(got, sizeof(got));
    if (n != (long)strlen(request)) {
        puts(request);
        fail_with(": the write of the call was refused", 1, -n);
    }
    fd = open_or_fail("/proc/acpi/call", O_RDONLY, "/proc/acpi/call did not open to read");
    n = syscall3(SYS_READ, fd, (long)got, (long)sizeof(got) - 1);
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (n <= 0 || got[n - 1] != 0 || !same(got, want)) {
        puts(request);
        puts(": the call's result is not ");
        puts(want);
        fail_with(", read", 1, n);
    }
    puts("EVPROBE: a call gave ");
    puts(want);
    puts("\n");
}

/** The run of `evprobe video`. */
static void video(void)
{
    int fd = open_or_fail(find_device("Video Bus"), O_RDONLY, "the video bus did not open");
    int codes[2] = {-1, -1};

    check_acpi_device(fd, PRODUCT_VIDEO, "LNXVIDEO/video/input0", KEY_BRIGHTNESSUP);
    call("\\RDOS", "0x0");
    call("\\RADD 2 0x28\n", "0x2a");
    call("\\RCAT \"ab\"", "\"abcd\"");
    call("\\RNON", "Error: AE_NOT_FOUND");
    call("\\RTST\n", "0x0");
    read_acpi_keys(fd, codes, 2);
    if (codes[0] != KEY_BRIGHTNESSDOWN || codes[1] != KEY_BRIGHTNESSUP) {
        fail_with("the video bus gave other keys", 1, (long)codes[0] << 16 | codes[1]);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
}

PROBE_SP_ENTRY;

/** Run the keyboard's, the mixer's or an ACPI device's checks, as the
 * argument says. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc >= 2 && begins(argv[1], "ps2")) {
        keyboard(0);
    } else if (argc >= 2 && begins(argv[1], "usbmouse")) {
        mouse(1);
    } else if (argc >= 2 && begins(argv[1], "usb")) {
        keyboard(1);
    } else if (argc >= 2 && begins(argv[1], "mouse")) {
        mouse(0);
    } else if (argc >= 3 && begins(argv[1], "mixer")) {
        mixer(argv[2]);
    } else if (argc >= 2 && begins(argv[1], "button")) {
        button();
    } else if (argc >= 2 && begins(argv[1], "video")) {
        video();
    } else if (argc >= 2 && begins(argv[1], "touchpad")) {
        touchpad();
    } else {
        fail("usage: evprobe ps2 | usb | mouse | usbmouse | mixer ID | button | video | touchpad");
    }
    puts("EVPROBE OK\n");
    sys_exit(0);
}
