/*
 * evdev.h: the input devices of /dev/input/eventN, Linux's event interface,
 * through which every keyboard, button and mouse reports to user space.
 */
#ifndef _EVDEV_H_
#define _EVDEV_H_

#include "lib/kint.h"
#include "fs/file.h"

/*
 * A driver registers each device it finds, with the keys, axes and
 * properties it has, and reports each key and movement through it;
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
#define EV_REL      0x02
#define EV_ABS      0x03
#define EV_MSC      0x04
#define EV_LED      0x11
#define EV_REP      0x14
#define SYN_REPORT  0
#define SYN_DROPPED 3
#define MSC_SCAN    4
#define LED_NUML    0 /* the bit numbers of EV_LED's codes */
#define LED_CAPSL   1
#define LED_SCROLLL 2
#define REP_DELAY   0 /* EV_REP's codes, each a time in milliseconds */
#define REP_PERIOD  1

/* EV_REL's codes; a wheel's REL_WHEEL counts detents, up positive, and its
 * REL_WHEEL_HI_RES the same turn in 120ths of a detent. */
#define REL_X             0x00
#define REL_Y             0x01 /* positive toward the user */
#define REL_HWHEEL        0x06 /* positive to the right */
#define REL_WHEEL         0x08
#define REL_WHEEL_HI_RES  0x0b
#define REL_HWHEEL_HI_RES 0x0c
#define REL_CNT           0x10

/* EV_ABS's codes: the ones below ABS_MT_SLOT, and the multitouch ones from
 * ABS_MT_FIRST to ABS_MT_LAST, each a value of every slot of a device that
 * keeps slots (`evdev_init_slots`), and of the slot ABS_MT_SLOT chose. */
#define ABS_X              0x00
#define ABS_Y              0x01
#define ABS_PRESSURE       0x18
#define ABS_TOOL_WIDTH     0x1c
#define ABS_MT_SLOT        0x2f
#define ABS_MT_FIRST       0x30
#define ABS_MT_POSITION_X  0x35
#define ABS_MT_POSITION_Y  0x36
#define ABS_MT_TOOL_TYPE   0x37
#define ABS_MT_TRACKING_ID 0x39 /* the contact's number, -1 for none */
#define ABS_MT_PRESSURE    0x3a
#define ABS_MT_LAST        0x3d
#define ABS_CNT            0x40

/* ABS_MT_TOOL_TYPE's value for a finger. */
#define MT_TOOL_FINGER 0

/* The properties of EVIOCGPROP. */
#define INPUT_PROP_POINTER      0x00 /* moves a pointer on a screen */
#define INPUT_PROP_DIRECT       0x01 /* touches the screen it points at */
#define INPUT_PROP_BUTTONPAD    0x02 /* a touchpad whose pad is its button */
#define INPUT_PROP_SEMI_MT      0x03 /* its slots bound the contacts' box */
#define INPUT_PROP_TOPBUTTONPAD 0x04 /* its buttons are a strip at the top */
#define INPUT_PROP_CNT          0x20

/* A mouse's buttons, EV_KEY codes. */
#define BTN_0       0x100
#define BTN_1       0x101
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112
#define BTN_SIDE    0x113
#define BTN_EXTRA   0x114
#define BTN_FORWARD 0x115
#define BTN_BACK    0x116

/* A touchpad's keys: a contact, and how many fingers touch, one to five. */
#define BTN_TOOL_FINGER    0x145
#define BTN_TOOL_QUINTTAP  0x148
#define BTN_TOUCH          0x14a
#define BTN_TOOL_DOUBLETAP 0x14d
#define BTN_TOOL_TRIPLETAP 0x14e
#define BTN_TOOL_QUADTAP   0x14f

/* What `evdev_describe` says a device reports beyond its keys. */
#define EVDEV_SCAN     0x01 /* MSC_SCAN, the code each key came from */
#define EVDEV_KEYBOARD 0x02 /* a keyboard's EV_REP and its three EV_LED LEDs */

/* An absolute axis's state and limits, Linux's `struct input_absinfo`,
 * which EVIOCGABS and EVIOCSABS copy: the same 24 bytes on both ABIs. */
struct input_absinfo {
    int32_t value; /* the last value reported */
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;       /* noise filtered out: a smaller change is dropped */
    int32_t flat;       /* the dead zone about the centre */
    int32_t resolution; /* units per millimetre, or per radian */
};

#define KEY_MUTE             113
#define KEY_VOLUMEDOWN       114
#define KEY_VOLUMEUP         115
#define KEY_POWER            116
#define KEY_PAUSE            119
#define KEY_SLEEP            142
#define KEY_WAKEUP           143
#define KEY_F16              186
#define KEY_CAMERA           212
#define KEY_BRIGHTNESSDOWN   224
#define KEY_BRIGHTNESSUP     225
#define KEY_SWITCHVIDEOMODE  227
#define KEY_WLAN             238
#define KEY_BRIGHTNESS_CYCLE 243
#define KEY_BRIGHTNESS_ZERO  244
#define KEY_DISPLAY_OFF      245
#define KEY_MICMUTE          248
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
 * kept by its pointer, and `flags`, EVDEV_SCAN and EVDEV_KEYBOARD, the
 * event types beyond EV_KEY that EVIOCGBIT says it reports. Called once,
 * after `evdev_register`. A NULL `dev` is ignored.
 */
void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags);

/** Give `dev` the version EVIOCGID gives, 0 until set. A NULL `dev` is
 * ignored. */
void evdev_set_version(struct evdev *dev, uint16_t version);

/**
 * Give `dev` the event `code` of `type`, EV_KEY or EV_REL, to report, as
 * Linux's `input_set_capability` does: a button, or a relative axis. Called
 * after `evdev_register`, before the device reports. A NULL `dev` is
 * ignored.
 */
void evdev_set_capability(struct evdev *dev, uint16_t type, uint16_t code);

/**
 * Give `dev` the absolute axis `code`, below ABS_CNT, with its limits and
 * its value 0, as Linux's `input_set_abs_params` does; EVIOCGABS then
 * answers for every axis of the device. Called after `evdev_register`,
 * before the device reports. A NULL `dev` is ignored.
 */
void evdev_set_abs(struct evdev *dev, uint16_t code, int32_t minimum, int32_t maximum, int32_t fuzz,
                   int32_t flat);

/** Give the absolute axis `code` of `dev` the resolution EVIOCGABS gives,
 * in units per millimetre, as Linux's `input_abs_set_res`. A NULL `dev` is
 * ignored. */
void evdev_set_abs_res(struct evdev *dev, uint16_t code, int32_t resolution);

/** Give `dev` the property `prop`, INPUT_PROP_*, which EVIOCGPROP lists.
 * A NULL `dev` is ignored. */
void evdev_set_prop(struct evdev *dev, unsigned int prop);

/* The most slots a device keeps, and the flags of `evdev_init_slots`,
 * Linux's INPUT_MT_* values. */
#define EVDEV_MT_SLOTS   5
#define INPUT_MT_POINTER 0x0001 /* a touchpad: the finger counts and pointer */
#define INPUT_MT_DIRECT  0x0002 /* a touchscreen */
#define INPUT_MT_TRACK   0x0008 /* `evdev_mt_assign_slots` matches contacts */
#define INPUT_MT_SEMI_MT 0x0010 /* INPUT_PROP_SEMI_MT */

/* A contact's place, for `evdev_mt_assign_slots`. */
struct evdev_mt_pos {
    int32_t x;
    int32_t y;
};

/**
 * Give `dev` `num_slots` slots, from 1 to EVDEV_MT_SLOTS, as Linux's
 * `input_mt_init_slots` does: the axes ABS_MT_SLOT and ABS_MT_TRACKING_ID,
 * every slot without a contact, and with INPUT_MT_POINTER or
 * INPUT_MT_DIRECT, BTN_TOUCH and ABS_X, ABS_Y and ABS_PRESSURE as copies of
 * the multitouch axes set before, without their fuzz; INPUT_MT_POINTER
 * adds the finger counts the slots can hold and INPUT_PROP_POINTER. Called
 * after the multitouch axes are set, before the device reports. Returns 0,
 * or -EINVAL for a count out of range or a second call with another.
 */
int evdev_init_slots(struct evdev *dev, int num_slots, unsigned int flags);

/**
 * Report the state of the slot an ABS_MT_SLOT event chose last, as Linux's
 * `input_mt_report_slot_state`: a contact, `active` nonzero, keeps its
 * tracking number or takes the next, and is reported with ABS_MT_TRACKING_ID
 * and ABS_MT_TOOL_TYPE `tool`; no contact is a tracking number of -1. The
 * slot counts as used in this frame (`evdev_mt_drop_unused`). Returns
 * whether the slot has a contact. Any context.
 */
int evdev_mt_report_slot_state(struct evdev *dev, int tool, int active);

/** Report `count` fingers with BTN_TOOL_FINGER to BTN_TOOL_QUINTTAP, as
 * Linux's `input_mt_report_finger_count`. */
void evdev_mt_report_finger_count(struct evdev *dev, int count);

/**
 * Report the pointer a program without slots reads, as Linux's
 * `input_mt_report_pointer_emulation`: BTN_TOUCH while any slot has a
 * contact, the place of the oldest contact in ABS_X and ABS_Y and its
 * pressure in ABS_PRESSURE, and with `use_count` the finger count of the
 * slots with contacts.
 */
void evdev_mt_report_pointer_emulation(struct evdev *dev, int use_count);

/** End the slots' frame, as Linux's `input_mt_drop_unused`: a contact in a
 * slot no `evdev_mt_report_slot_state` named since the last call is
 * lifted. */
void evdev_mt_drop_unused(struct evdev *dev);

/**
 * Fill `slots` with a slot for each of the `num_pos` contacts at `pos`, as
 * Linux's `input_mt_assign_slots`: the assignment that keeps each contact
 * nearest the one its slot had, within `dmax` units, 0 for any distance;
 * a new contact takes a free slot. Returns 0, -ENXIO for a device whose
 * slots do not track (INPUT_MT_TRACK), -EINVAL for more contacts than
 * slots.
 */
int evdev_mt_assign_slots(struct evdev *dev, int *slots, const struct evdev_mt_pos *pos,
                          int num_pos, int dmax);

/** Copy the state and limits of the absolute axis `code` of `dev` into
 * `*out`. Returns 0, or -EINVAL for an axis the device does not have. */
int evdev_get_abs(struct evdev *dev, uint16_t code, struct input_absinfo *out);

/* A handler of a device's events in the kernel, as a handler of Linux's
 * input core: mouse.c's, which makes a touchpad's packets for the mouse
 * device. */
typedef void (*evdev_handler_t)(struct evdev *dev, uint16_t type, uint16_t code, int32_t value);

/**
 * Give `dev` the handler `fn`, called after its opens get each event that
 * passes the input core's filters, with the value they get, unless an open
 * holds the grab, which takes the events from the handler too, as Linux's
 * grab does. It is called in the reporter's context, without the evdev
 * lock. A NULL `dev` is ignored.
 */
void evdev_set_handler(struct evdev *dev, evdev_handler_t fn);

/**
 * Take `dev` away: its node goes from /dev/input, a reader of an open of it
 * is woken and gets -ENODEV, and its slot is free for another device once
 * the last open closes. Any context. A NULL `dev` is ignored.
 */
void evdev_unregister(struct evdev *dev);

/**
 * Report one event of `dev`: its type, code and value as Linux's
 * `struct input_event` has them, queued for every open of the device, or
 * only for the open that holds the grab; an EV_SYN SYN_REPORT ends a packet
 * and wakes the readers. Every event of a packet has the time of its first,
 * on the clock each open chose (EVIOCSCLOCKID). The input core's filters
 * apply, as Linux's: an event of a key or axis the device did not register
 * is dropped, as are a key's press while it is down and its release while
 * it is up (a repeat, value 2, passes), a relative motion of 0, and an
 * absolute value within the axis's fuzz of the last one. An open does not
 * get the events its masks leave out (EVIOCSMASK), nor a SYN_REPORT that
 * ends a packet it got nothing of. Any context, an interrupt handler among
 * them; it does not sleep. A NULL `dev`, a device whose registration
 * failed, is ignored.
 */
void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value);

/**
 * Report a keyboard's key as one packet: MSC_SCAN with `scan`, the code the
 * keyboard sent, then the key `code` down (`down` nonzero) or up, then
 * SYN_REPORT. A `code` of 0, a key the driver's table does not name, gives
 * the MSC_SCAN alone, so that a user can name the key from its code.
 */
void evdev_report_key(struct evdev *dev, uint32_t scan, uint16_t code, int down);

/**
 * Light the LEDs of the keyboard `dev`, `mask` a bit 1 << LED_* for each LED
 * on: an EV_LED event for each LED that changes, then SYN_REPORT, as Linux's
 * evdev reports the console's LEDs; EVIOCGLED gives the mask. Any context.
 * A NULL `dev` or one that is not a keyboard is ignored.
 */
void evdev_report_leds(struct evdev *dev, unsigned int mask);

/**
 * Give the keyboard `dev` the repeat's times in milliseconds: an EV_REP
 * event, REP_DELAY or REP_PERIOD, for each that changes, then SYN_REPORT, as
 * Linux's input core reports a change of the repeat. Any context. A NULL
 * `dev` or one that is not a keyboard is ignored.
 */
void evdev_report_repeat(struct evdev *dev, int delay_ms, int period_ms);

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
 * smaller than one record, -EFAULT, -EINTR, or -ENODEV once the device has
 * gone or the open was revoked (EVIOCREVOKE).
 */
int32_t evdev_read(fd_entry_t *e, char __user *buf, uint32_t count);

/** The `poll` of an open device: POLLIN while its queue has a record, and
 * POLLHUP with POLLERR once the device has gone or the open was revoked,
 * with `*wq` the queue's wait queue. */
short evdev_poll(fd_entry_t *e, short events, struct wait_queue **wq);

/**
 * The ioctls of an open device, with Linux's numbers and answers:
 * EVIOCGVERSION, EVIOCGID, EVIOCGNAME, EVIOCGPHYS, EVIOCGUNIQ, EVIOCGPROP,
 * EVIOCGBIT, EVIOCGKEY, EVIOCGLED, EVIOCGSND, EVIOCGSW, EVIOCGABS and
 * EVIOCSABS, EVIOCGMTSLOTS, EVIOCGRAB, EVIOCREVOKE, EVIOCSCLOCKID,
 * EVIOCGMASK and EVIOCSMASK, and on a keyboard EVIOCGREP and EVIOCSREP,
 * which read and set every keyboard's repeat. -ENODEV on a device that has
 * gone or an open that was revoked, -EINVAL for another request, -EFAULT
 * for a bad pointer, -ENOSYS for the repeat's requests on a device that is
 * no keyboard.
 */
int32_t evdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

/** Release the queue of the open `e`, and its grab if it holds one. */
void evdev_close(fd_entry_t *e);

#endif /* _EVDEV_H_ */
