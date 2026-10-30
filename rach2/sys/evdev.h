/*
 * evdev.h: the input devices of /dev/input/eventN, Linux's event interface,
 * through which every keyboard and button reports its keys to user space.
 */
#ifndef _EVDEV_H_
#define _EVDEV_H_

#include "lib/kint.h"
#include "fs/file.h"

/*
 * A driver registers each device it finds and reports each key through it;
 * a reader opens the device's node and reads Linux's `struct input_event`
 * records, each open with a queue of its own. The node of device N is
 * /dev/input/eventN, major 13 and minor 64 + N, as on Linux, and is listed
 * only while N is registered. devfs.c owns the nodes and calls the
 * operations below.
 */

/* The most devices, event0 to event31, and their nodes' numbers. */
#define EVDEV_MAX        32
#define EVDEV_MAJOR      13
#define EVDEV_MINOR_BASE 64

/* The bus types of EVIOCGID, Linux's BUS_ values. */
#define EVDEV_BUS_USB   0x03
#define EVDEV_BUS_I8042 0x11
#define EVDEV_BUS_HOST  0x19

/* The bytes of a set of key codes, one bit for each of Linux's KEY_CNT,
 * 0x300, codes, as EVIOCGBIT(EV_KEY) and EVIOCGKEY give it. */
#define EVDEV_KEY_BYTES 96

struct evdev;

/* The event types, codes and keys the kernel's drivers report, with
 * Linux's numbers (<linux/input-event-codes.h>). */
#define EV_SYN      0x00
#define EV_KEY      0x01
#define EV_MSC      0x04
#define SYN_REPORT  0
#define SYN_DROPPED 3
#define MSC_SCAN    4

#define KEY_MUTE             113
#define KEY_VOLUMEDOWN       114
#define KEY_VOLUMEUP         115
#define KEY_POWER            116
#define KEY_PAUSE            119
#define KEY_SLEEP            142
#define KEY_WAKEUP           143
#define KEY_BRIGHTNESSDOWN   224
#define KEY_BRIGHTNESSUP     225
#define KEY_SWITCHVIDEOMODE  227
#define KEY_BRIGHTNESS_CYCLE 243
#define KEY_BRIGHTNESS_ZERO  244
#define KEY_DISPLAY_OFF      245
#define KEY_MAX              0x2ff

/**
 * Register an input device and give it the lowest free node. `name` is the
 * text EVIOCGNAME gives, such as "AT Translated Set 2 keyboard", kept by
 * its pointer; `bustype`, `vendor` and `product` are what EVIOCGID gives;
 * `keys`, of EVDEV_KEY_BYTES bytes, has a bit set for each key code the
 * device can report, and is copied. Any context: the devices are a static
 * table, under a spinlock taken with interrupts off. Returns the device, or
 * NULL when all EVDEV_MAX nodes are taken.
 */
struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys);

/**
 * Give `dev` the text EVIOCGPHYS gives, such as "isa0060/serio0/input0",
 * kept by its pointer, and when `scan` is nonzero say that it reports
 * MSC_SCAN, as a keyboard does. Called once, after `evdev_register`. A NULL
 * `dev` is ignored.
 */
void evdev_describe(struct evdev *dev, const char *phys, int scan);

/**
 * Take `dev` away: its node goes from /dev/input, a reader of an open of it
 * is woken and gets -ENODEV, and its slot is free for another device once
 * the last open closes. Any context. A NULL `dev` is ignored.
 */
void evdev_unregister(struct evdev *dev);

/**
 * Report one event of `dev`: its type, code and value as Linux's
 * `struct input_event` has them, stamped with the real time now and queued
 * for every open of the device, or only for the open that holds the grab;
 * an EV_SYN SYN_REPORT ends a packet and wakes the readers. An EV_KEY of a
 * key the device did not register is dropped, as is a release of a key that
 * is up; a press of a key that is down becomes a repeat, value 2. Any
 * context, an interrupt handler among them; it does not sleep. A NULL
 * `dev`, a device whose registration failed, is ignored.
 */
void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value);

/**
 * Report a keyboard's key as one packet: MSC_SCAN with `scan`, the code the
 * keyboard sent, then the key `code` down (`down` nonzero) or up, then
 * SYN_REPORT. A `code` of 0, a key the driver's table does not name, gives
 * the MSC_SCAN alone, so that a user can name the key from its code.
 */
void evdev_report_key(struct evdev *dev, uint32_t scan, uint16_t code, int down);

/** Whether device `n`, from 0, is registered now: 1 or 0. */
int evdev_present(int n);

/**
 * Open device `n` on the new descriptor `e`: give it a queue of its own,
 * from the heap, in `e->priv`. devfs calls it before it binds the
 * descriptor to the operations below, and frees the descriptor when it
 * fails. Returns 0, -ENODEV for a device that is not registered, or
 * -ENOMEM.
 */
int32_t evdev_open(fd_entry_t *e, int n);

/**
 * Read whole records into `buf`: as many as fit in `count` bytes, in the
 * caller's layout, 16 bytes each on i686 and 24 on x86-64. A reader that
 * fell behind finds the oldest records dropped and a SYN_DROPPED in their
 * place. Sleeps while the queue is empty, unless the description is
 * nonblocking (-EAGAIN). Returns the bytes read, -EINVAL for a `count`
 * smaller than one record, -EFAULT, or -EINTR.
 */
int32_t evdev_read(fd_entry_t *e, char __user *buf, uint32_t count);

/** The `poll` of an open device: POLLIN while its queue has a record, with
 * `*wq` the queue's wait queue. */
short evdev_poll(fd_entry_t *e, short events, struct wait_queue **wq);

/**
 * The ioctls of an open device: EVIOCGVERSION, EVIOCGID, EVIOCGNAME,
 * EVIOCGPHYS, EVIOCGBIT, EVIOCGKEY and EVIOCGRAB, with Linux's numbers and
 * answers. -ENOTTY for another request, -EFAULT for a bad pointer.
 */
int32_t evdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

/** Release the queue of the open `e`, and its grab if it holds one. */
void evdev_close(fd_entry_t *e);

#endif /* _EVDEV_H_ */
