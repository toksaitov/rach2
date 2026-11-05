/* rfkill.h: the switches that keep radios off, with Linux's /dev/rfkill
 * events and /sys/class/rfkill attributes for user space. */
#ifndef _RFKILL_H_
#define _RFKILL_H_

#include "lib/kint.h"
#include "fs/file.h"

/* Linux's switch types (uapi/linux/rfkill.h). RFKILL_TYPE_ALL names every
 * type in an event a program writes; no switch has it. */
#define RFKILL_TYPE_ALL       0
#define RFKILL_TYPE_WLAN      1
#define RFKILL_TYPE_BLUETOOTH 2
#define RFKILL_TYPE_UWB       3
#define RFKILL_TYPE_WIMAX     4
#define RFKILL_TYPE_WWAN      5
#define RFKILL_TYPE_GPS       6
#define RFKILL_TYPE_FM        7
#define RFKILL_TYPE_NFC       8
#define RFKILL_NUM_TYPES      9

/* An event's operation: a switch came or went, one switch changed, or, in a
 * write, every switch of a type is to change. */
#define RFKILL_OP_ADD        0
#define RFKILL_OP_DEL        1
#define RFKILL_OP_CHANGE     2
#define RFKILL_OP_CHANGE_ALL 3

/* An event on /dev/rfkill is Linux's `struct rfkill_event`, eight bytes:
 * the switch's index as a host-order 32-bit word, then its type, the
 * operation, and the soft and hard blocks as 0 or 1. The extended form,
 * `struct rfkill_event_ext`, adds the hard block's reasons in a ninth byte;
 * a reader gets it after RFKILL_IOCTL_MAX_SIZE raises its size. */
#define RFKILL_EVENT_SIZE_V1  8
#define RFKILL_EVENT_SIZE_EXT 9

/* The reasons for a hard block, in the extended event and the attribute
 * `hard_block_reasons`: a switch or key of the machine, or another owner
 * of a shared radio. */
#define RFKILL_HARD_BLOCK_SIGNAL    0x01
#define RFKILL_HARD_BLOCK_NOT_OWNER 0x02

/* The requests of /dev/rfkill: _IO('R', 1), which turns off the keys'
 * handling of the switches and is accepted with nothing to turn off, and
 * _IOW('R', 2, __u32), the event size this open reads and writes. */
#define RFKILL_IOCTL_NOINPUT  0x5201u
#define RFKILL_IOCTL_MAX_SIZE 0x40045202u

/* The value of the attribute `state`, Linux's RFKILL_USER_STATE_. */
#define RFKILL_STATE_SOFT_BLOCKED 0
#define RFKILL_STATE_UNBLOCKED    1
#define RFKILL_STATE_HARD_BLOCKED 2

/* The attributes of /sys/class/rfkill/rfkillN, in the order the directory
 * lists them; `soft` and `state` take a privileged write. */
#define RFKILL_ATTR_HARD    0
#define RFKILL_ATTR_REASONS 1 /* hard_block_reasons */
#define RFKILL_ATTR_INDEX   2
#define RFKILL_ATTR_NAME    3
#define RFKILL_ATTR_PERSIST 4 /* persistent */
#define RFKILL_ATTR_SOFT    5
#define RFKILL_ATTR_STATE   6
#define RFKILL_ATTR_TYPE    7
#define RFKILL_ATTR_UEVENT  8
#define RFKILL_ATTRS        9

#define RFKILL_MAX      16 /* switches registered at once */
#define RFKILL_NAME_MAX 32 /* a name's bytes with its NUL */
#define RFKILL_QUEUE    32 /* events an open holds; a new one drops the oldest */

struct rfkill;

/* What a switch's owner does when the soft block changes. */
struct rfkill_ops {
    /**
     * Keep the radio off when `blocked` is nonzero and let it run when it
     * is zero; `blocked` is the soft block alone, the hard block being the
     * owner's own report. Process context under the kernel lock, without
     * net_lock; the owner may queue the change and return before it is
     * done. Returns 0, or a negative errno, which leaves the soft block as
     * it was and sends no event.
     */
    int (*set_block)(void *data, int blocked);
};

/**
 * Register a switch named `name`, of `type`, which belongs to the radio
 * `phy` (phyN), or to none when `phy` is -1. It takes the next index, which
 * is never reused, starts soft-blocked when the last RFKILL_OP_CHANGE_ALL
 * for its type blocked them, and is announced to every open of /dev/rfkill
 * with RFKILL_OP_ADD. The name is copied; `ops` and `data` are borrowed
 * until `rfkill_unregister`. The initial soft block calls no `set_block`:
 * the owner reads `rfkill_soft_blocked` once this returns.
 *
 * Process context under the kernel lock. Returns 0 and `*out`, -EINVAL for
 * an empty or too long name, RFKILL_TYPE_ALL, an unknown type or no
 * `set_block`, or -ENOSPC when RFKILL_MAX switches are registered.
 */
int rfkill_register(const char *name, unsigned type, int phy, const struct rfkill_ops *ops,
                    void *data, struct rfkill **out);

/** Withdraw a switch, sending RFKILL_OP_DEL; no `set_block` follows. Process
 * context under the kernel lock; NULL and a second call do nothing. */
void rfkill_unregister(struct rfkill *rf);

/** Record the hard block the owner's hardware reports, sending
 * RFKILL_OP_CHANGE when it changed. Any context, an interrupt handler among
 * them; it does not sleep and calls no `set_block`. Returns whether the
 * switch is now blocked, soft or hard. */
int rfkill_set_hw_state(struct rfkill *rf, int blocked);

/** Whether the soft block is set; any context. */
int rfkill_soft_blocked(const struct rfkill *rf);

/** The switch's index, the N of rfkillN. */
unsigned rfkill_index(const struct rfkill *rf);

/** The index of the `n`th switch, counting from 0 in the order of their
 * indexes, or -1 past the last: what /sys/class/rfkill lists. */
int rfkill_nth(int n);

/** Whether switch `index` is registered. */
int rfkill_present(int index);

/** The index of the switch the `len` bytes at `name` name as `rfkillN`,
 * without leading zeros, or -1 when there is no such switch. */
int rfkill_lookup(const char *name, int len);

/** The radio switch `index` belongs to, or -1 for none or no switch: the
 * target of /sys/class/rfkill/rfkillN/device, which is the switch itself
 * when it has no radio. */
int rfkill_phy(int index);

/** Write attribute `attr` (RFKILL_ATTR_*) of switch `index` into `out` of
 * `cap` bytes as Linux spells it, with its final newline. Returns its
 * length, -ENOENT for no such switch or attribute, or -ENOSPC when it does
 * not fit; `out` is not NUL-terminated. */
int rfkill_show(int index, int attr, char *out, int cap);

/**
 * Write the `len` bytes at `text`, without a final newline, to attribute
 * `attr` of switch `index`: `soft` takes 0 or 1, `state` takes 0, which
 * blocks, or 1, which unblocks, as Linux's do. Process context under the
 * kernel lock; the caller has checked privilege. Returns 0, -ENOENT for no
 * such switch, -EINVAL for another attribute or value, or the error of the
 * owner's `set_block`.
 */
int rfkill_store(int index, int attr, const char *text, int len);

/* /dev/rfkill. An open is queued RFKILL_OP_ADD for each switch in the
 * order of their indexes, then each later event; each read takes one event,
 * cut to the count and to the open's event size, 8 bytes until
 * RFKILL_IOCTL_MAX_SIZE. A read with nothing queued waits, or fails with
 * EAGAIN under O_NONBLOCK. A write of at least 7 bytes is one event:
 * RFKILL_OP_CHANGE sets the soft block of the switch of that index and
 * type, RFKILL_OP_CHANGE_ALL that of every switch of the type, and of every
 * switch for RFKILL_TYPE_ALL; it returns the bytes taken, at most the
 * event size, or EINVAL for another operation or an unknown type. */

/** Make the open's event queue on `e`; -ENOMEM. */
int32_t rfkill_open(fd_entry_t *e);
/** Take one event; see above. */
int32_t rfkill_read(fd_entry_t *e, char __user *buf, uint32_t count);
/** Apply one event; see above. */
int32_t rfkill_write(fd_entry_t *e, const char __user *buf, uint32_t count);
/** Readable while an event is queued; always writable. */
short rfkill_poll(fd_entry_t *e, short events, struct wait_queue **wq);
/** RFKILL_IOCTL_NOINPUT, and RFKILL_IOCTL_MAX_SIZE from 8 to 255 bytes, of
 * which events use 9; -EINVAL, -EFAULT, or -ENOTTY for another request. */
int32_t rfkill_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
/** Free the open's queue. */
void rfkill_close(fd_entry_t *e);

#endif /* _RFKILL_H_ */
