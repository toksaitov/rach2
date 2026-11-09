/*
 * pin.c: page frames held for a device, and charged against RLIMIT_MEMLOCK,
 * as pin.h says.
 */

#include "pin.h"

#include "kheap.h"
#include "mm.h"
#include "notify.h"
#include "paging.h"
#include "../lib/kerrno.h"
#include "../proc/process.h"

/* A pin: the frames it holds, the address space they are charged to, and
 * the range it hears of unmaps on. */
struct pin {
    struct mmn_range note; /* first, so that the range is the record */
    struct mm *mm;         /* charged to, or NULL for no one */
    const struct pin_holder *holder;
    uintptr_t *frames; /* the physical address of each page */
    uint32_t count;
    uint32_t live; /* 1 while the frames are held */
};

/** Drop what `pin` holds, once: its range's registration, its frames'
 * pins and its charge. */
static void pin_drop(struct pin *pin)
{
    uint32_t i;

    if (!pin->live) {
        return;
    }
    pin->live = 0;
    mmn_unregister(&pin->note);
    for (i = 0; i < pin->count; i++) {
        frame_unpin(pin->frames[i]);
    }
    if (pin->mm) {
        pin->mm->pinned -= pin->count;
        pin->mm = NULL;
    }
}

/** The pin's range heard of a change: an unmap of a page of a user range,
 * or the end of the address space, takes the pin back after the holder
 * has stopped the device. Other changes leave the pinned frames as they
 * are, as Linux's long-term pins do. */
static void pin_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct pin *pin = (struct pin *)r;

    (void)start;
    (void)end;
    if (event != MMN_UNMAP && event != MMN_RELEASE) {
        return;
    }
    if (pin->holder && pin->holder->release) {
        pin->holder->release(pin, pin->holder->owner);
    }
    pin_drop(pin);
}

static const struct mmn_ops pin_note_ops = {pin_invalidate};

/** A new record of `count` pages with room for their addresses, charged to
 * `mm` when it is not NULL and the caller's RLIMIT_MEMLOCK has room: 0 with
 * it in `*out`, or -ENOMEM. Nothing is pinned yet. */
static int pin_new(struct mm *mm, uint32_t count, const struct pin_holder *holder, struct pin **out)
{
    struct pin *pin;

    if (mm && !mem_lock_room(mm->pinned, count)) {
        return -ENOMEM;
    }
    pin = kzalloc(sizeof(*pin));
    if (!pin) {
        return -ENOMEM;
    }
    pin->frames = kmalloc((size_t)count * sizeof(uintptr_t));
    if (!pin->frames) {
        kfree(pin);
        return -ENOMEM;
    }
    pin->holder = holder;
    pin->count = count;
    *out = pin;
    return 0;
}

/** Charge the pinned `pin` to `mm` and register its range [start, end),
 * empty for an object's frames, so that the holder hears of the end. */
static void pin_start(struct pin *pin, struct mm *mm, uintptr_t start, uintptr_t end)
{
    pin->live = 1;
    if (mm) {
        pin->mm = mm;
        mm->pinned += pin->count;
        mmn_register(mm, &pin->note, start, end, &pin_note_ops, pin);
    }
}

/** The record is made before the pages are brought in, which may sleep, and
 * the charge is checked again after, since another thread of the address
 * space may have pinned meanwhile. */
int pin_user(struct mm *mm, uintptr_t addr, uintptr_t len, uint32_t flags,
             const struct pin_holder *holder, struct pin **out)
{
    uintptr_t start = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t end = addr + len;
    struct pin *pin;
    int rc;

    if (len == 0 || end < addr) {
        return -EINVAL;
    }
    end = (end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    if (end < start) {
        return -EINVAL;
    }
    if (start < USER_ADDR_MIN || end > USER_ADDR_MAX) {
        return -EFAULT;
    }
    if ((end - start) / PAGE_SIZE > 0xFFFFFFFFu) {
        return -ENOMEM;
    }
    rc = pin_new(mm, (uint32_t)((end - start) / PAGE_SIZE), holder, &pin);
    if (rc != 0) {
        return rc;
    }
    rc = paging_pin_range(mm, start, end, (flags & PIN_WRITE) != 0, pin->frames);
    if (rc == 0 && !mem_lock_room(mm->pinned, pin->count)) {
        uint32_t i;

        for (i = 0; i < pin->count; i++) {
            frame_unpin(pin->frames[i]);
        }
        rc = -ENOMEM;
    }
    if (rc != 0) {
        kfree(pin->frames);
        kfree(pin);
        return rc;
    }
    pin_start(pin, mm, start, end);
    *out = pin;
    return 0;
}

int pin_frames(struct mm *mm, const uintptr_t *frames, uint32_t count,
               const struct pin_holder *holder, struct pin **out)
{
    struct pin *pin;
    uint32_t i;
    int rc;

    if (count == 0) {
        return -EINVAL;
    }
    rc = pin_new(mm, count, holder, &pin);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        pin->frames[i] = frames[i] & ~(uintptr_t)(PAGE_SIZE - 1);
        if (frame_pin(pin->frames[i]) != 0) {
            while (i--) {
                frame_unpin(pin->frames[i]);
            }
            kfree(pin->frames);
            kfree(pin);
            return -EINVAL;
        }
    }
    pin_start(pin, mm, 0, 0);
    *out = pin;
    return 0;
}

void unpin(struct pin *pin)
{
    if (!pin) {
        return;
    }
    pin_drop(pin);
    kfree(pin->frames);
    kfree(pin);
}

int pin_live(const struct pin *pin)
{
    return pin->live != 0;
}

uint32_t pin_count(const struct pin *pin)
{
    return pin->count;
}

uintptr_t pin_phys(const struct pin *pin, uint32_t i)
{
    return pin->live && i < pin->count ? pin->frames[i] : 0;
}
