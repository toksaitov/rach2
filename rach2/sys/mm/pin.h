/*
 * pin: page frames held for a device, which GPU drivers, KVM and VFIO use
 * as Linux's pin_user_pages and account_locked_vm. A pin keeps each frame
 * out of the pool whatever mappings go (`frame_pin`), so a device may read
 * and write it by its physical address, and counts its pages against
 * RLIMIT_MEMLOCK in the address space it is charged to, VmPin of
 * /proc/PID/status.
 *
 * A pin of a user range holds the frames a process's pages have, each made
 * the address space's own first, so that a later write by the process
 * reaches the frame the device has; a fork after it gives the child copies
 * of the pinned private pages. A pin of an object's frames holds frames a
 * driver allocated, a buffer's, charged to a process when one is named.
 *
 * A pin goes in one of three ways. `unpin`, by its holder, drops the
 * frames and the charge and frees the record. An unmap of any page of a
 * user range, or the end of the address space a pin is charged to, first
 * calls the holder's `release`, which stops the device's use of the
 * frames, and then drops them and the charge; the record stays the
 * holder's, which `pin_live` then answers 0 for, until its `unpin`. Every
 * call is made under the big kernel lock.
 */
#ifndef _PIN_H_
#define _PIN_H_

#include "../lib/kint.h"

struct mm;
struct pin;

/* A flag of `pin_user`: the device writes the pages, which the range must
 * allow, so each is brought in for a write. */
#define PIN_WRITE 0x1u

/*
 * The holder of a pin, a record of the driver's that lives as long as the
 * pin: `release` is what it does when the memory manager takes the pin's
 * frames back, called with `owner`, and must stop the device's use of
 * every frame before it returns. It is called under the big kernel lock
 * from an unmap or the end of an address space, must not sleep, and must
 * not unpin or pin. A NULL holder, or a NULL `release`, is told nothing.
 */
struct pin_holder {
    void (*release)(struct pin *pin, void *owner);
    void *owner;
};

/**
 * Pin the pages of [addr, addr + len) of `mm`, the range rounded out to
 * whole pages, for a device: each page is brought in as a fault would, for
 * a write with PIN_WRITE, given a frame of the address space's own when it
 * is private and shared with another, and its frame pinned. The pages count
 * in `mm`'s VmPin against the caller's RLIMIT_MEMLOCK (`mem_lock_room`).
 * `holder` is told when an unmap or the end of `mm` takes the pin back.
 * May sleep. 0 with the record in `*out`; -EINVAL for an empty
 * or wrapping range; -EFAULT for a page outside user space, in no region,
 * of device memory or without the access; -ENOMEM past RLIMIT_MEMLOCK, as
 * Linux's account_locked_vm answers, or when memory runs out. On an error
 * nothing is pinned.
 */
int pin_user(struct mm *mm, uintptr_t addr, uintptr_t len, uint32_t flags,
             const struct pin_holder *holder, struct pin **out);

/**
 * Pin the `count` frames whose physical addresses `frames` gives, which the
 * frame allocator counts, for a device. With `mm` the pages are charged to
 * it as `pin_user`'s are, against the caller's RLIMIT_MEMLOCK, and the end
 * of `mm` takes the pin back, telling `holder`; with NULL they are charged
 * to no one and go only at `unpin`. Does not sleep. 0 with the record in
 * `*out`; -EINVAL for no frames or one the allocator does not count or
 * cannot pin again; -ENOMEM past RLIMIT_MEMLOCK or with no memory for the
 * record. On an error nothing is pinned.
 */
int pin_frames(struct mm *mm, const uintptr_t *frames, uint32_t count,
               const struct pin_holder *holder, struct pin **out);

/** Drop what `pin` still holds, its frames and its charge, and free the
 * record; NULL is ignored. */
void unpin(struct pin *pin);

/** Whether `pin` still holds its frames: 1 until an unmap or the end of
 * its address space took them back. */
int pin_live(const struct pin *pin);

/** The pages `pin` holds, or held. */
uint32_t pin_count(const struct pin *pin);

/** The physical address of page `i` of `pin`, or 0 past its end or once
 * it is no longer live. */
uintptr_t pin_phys(const struct pin *pin, uint32_t i);

/**
 * Pin the frames of the pages of [start, end) of `mm`, page-aligned, as
 * `pin_user` says, and put their physical addresses in `frames`, one for
 * each page. The part of paging.c that walks the regions and the tables.
 * 0, -EFAULT or -ENOMEM; on an error nothing is pinned.
 */
int paging_pin_range(struct mm *mm, uintptr_t start, uintptr_t end, int write, uintptr_t *frames);

#endif /* _PIN_H_ */
