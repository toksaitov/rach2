/*
 * pin.c: page frames held for a device, and charged against RLIMIT_MEMLOCK,
 * as pin.h says.
 */

#include "pin.h"

#include "filemap.h"
#include "kheap.h"
#include "mm.h"
#include "notify.h"
#include "paging.h"
#include "vm.h"
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
    uint32_t ram_pages; /* frames pinned and charged, excluding device pages */
    uint32_t live;      /* 1 while the frames are held */
    /* Of a holder with PIN_KEEP_PAST_END: the shared mappings of disk files
     * the range met, each held while the pin is live, or NULL for none. */
    struct vm_file **files;
    uint32_t nfiles;
};

/** Drop what `pin` holds, once: its range's registration, its frames'
 * pins, its charge and its files, whose last reference puts each on the
 * list `vm_file_reap` empties. */
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
        pin->mm->pinned -= pin->ram_pages;
        pin->mm = NULL;
    }
    for (i = 0; i < pin->nfiles; i++) {
        vm_file_put(pin->files[i]);
    }
    kfree(pin->files);
    pin->files = NULL;
    pin->nfiles = 0;
}

/** Whether `f` is among the first `n` of `files`. */
static int pin_file_listed(struct vm_file *const *files, uint32_t n, const struct vm_file *f)
{
    while (n--) {
        if (files[n] == f) {
            return 1;
        }
    }
    return 0;
}

/**
 * Hold each shared mapping of a disk file that [start, end) of `mm` meets,
 * once, in `pin`, as Linux's pin holds a page cache folio: the file's pages
 * in memory, among them the frames pinned, go with its last mapping
 * (`filemap_disk_shared`), and a pin kept past the end of `mm` would then
 * hold frames that a later write(2) of the file no longer reaches. Held,
 * the mapping keeps its pages, the file's descriptor writes reach them, and
 * its release after `unpin` writes back what the process stored in them.
 * The ranges of the regions met cannot change without an unmap, which
 * takes the pin back. Does not sleep. 0, or -ENOMEM with nothing held.
 */
static int pin_keep_files(struct pin *pin, struct mm *mm, uintptr_t start, uintptr_t end)
{
    const vma_t *r;
    uint32_t n = 0;

    for (r = vm_lower(&mm->vm, start); r && r->start < end; r = vm_next(r)) {
        n += filemap_disk_pages(r->file);
    }
    if (n == 0) {
        return 0;
    }
    pin->files = kmalloc((size_t)n * sizeof(*pin->files));
    if (!pin->files) {
        return -ENOMEM;
    }
    for (r = vm_lower(&mm->vm, start); r && r->start < end; r = vm_next(r)) {
        if (filemap_disk_pages(r->file) && !pin_file_listed(pin->files, pin->nfiles, r->file)) {
            vm_file_get(r->file);
            pin->files[pin->nfiles++] = r->file;
        }
    }
    return 0;
}

/** Whether [start, end) of `mm` meets a writable shared disk mapping.
 * Linux's writable_file_mapping_allowed refuses a long-term write pin of
 * one, so pin_user refuses before taking frames or backing-file holds.
 * Does not sleep. */
static int pin_meets_tracked_file(struct mm *mm, uintptr_t start, uintptr_t end)
{
    const vma_t *r;

    for (r = vm_lower(&mm->vm, start); r && r->start < end; r = vm_next(r)) {
        if ((r->prot & VM_PROT_WRITE) && filemap_disk_pages(r->file)) {
            return 1;
        }
    }
    return 0;
}

/** The pin's range heard of a change: an unmap of a page of a user range,
 * or the end of the address space, takes the pin back after the holder
 * has stopped the device. A pin with device pages also ends before their
 * revocation or protection change; RAM pins survive those changes. At the
 * end, a pin whose holder keeps it past the end, device pages and all,
 * loses only its charge, its range already off the list. */
static void pin_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct pin *pin = (struct pin *)r;

    (void)start;
    (void)end;
    if (event != MMN_UNMAP && event != MMN_RELEASE &&
        !(pin->ram_pages != pin->count && (event == MMN_TRUNCATE || event == MMN_PROTECT))) {
        return;
    }
    if (event == MMN_RELEASE && pin->holder && (pin->holder->flags & PIN_KEEP_PAST_END)) {
        if (pin->mm) {
            pin->mm->pinned -= pin->ram_pages;
            pin->mm = NULL;
        }
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
    pin->ram_pages = count;
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
        mm->pinned += pin->ram_pages;
        mmn_register(mm, &pin->note, start, end, &pin_note_ops, pin);
    }
}

/** The pin of `pin_user`, of the page-aligned [start, end), under `mm`'s
 * regions' lock held to read once, which the bringing in of a page may let
 * go and take again. */
static int pin_user_locked(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t flags,
                           const struct pin_holder *holder, struct pin **out)
{
    uintptr_t pages = (end - start) / PAGE_SIZE;
    struct pin *pin;
    int rc;

    if ((flags & (PIN_LONGTERM | PIN_WRITE)) == (PIN_LONGTERM | PIN_WRITE) &&
        pin_meets_tracked_file(mm, start, end)) {
        return -EFAULT;
    }
    rc = pin_new((flags & PIN_IO) ? NULL : mm, (uint32_t)pages, holder, &pin);
    if (rc != 0) {
        return rc;
    }
    if (flags & PIN_IO) {
        rc = paging_dma_range(mm, start, end, flags & (PIN_WRITE | PIN_SLOW), pin->frames,
                              &pin->ram_pages);
    } else {
        rc = paging_pin_range(mm, start, end, flags & (PIN_WRITE | PIN_SLOW), pin->frames);
    }
    if (rc == 0) {
        if ((flags & (PIN_LONGTERM | PIN_WRITE)) == (PIN_LONGTERM | PIN_WRITE) &&
            pin_meets_tracked_file(mm, start, end)) {
            rc = -EFAULT;
        } else if ((pin->ram_pages != 0 && !mem_lock_room(mm->pinned, pin->ram_pages)) ||
                   (holder && (holder->flags & PIN_KEEP_PAST_END) &&
                    pin_keep_files(pin, mm, start, end) != 0)) {
            rc = -ENOMEM;
        }
        if (rc != 0) {
            uint32_t i;

            for (i = 0; i < pin->count; i++) {
                frame_unpin(pin->frames[i]);
            }
        }
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

/** The record is made before the pages are brought in, which may sleep, and
 * the charge and the regions a long-term write pin refuses are checked
 * again after, since another thread of the address space may have pinned
 * or mapped meanwhile. The regions' lock is held to read across the pin
 * (`mm_read_begin`), so the caller holds none of `mm`'s. */
int pin_user(struct mm *mm, uintptr_t addr, uintptr_t len, uint32_t flags,
             const struct pin_holder *holder, struct pin **out)
{
    uintptr_t start = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t end = addr + len;
    uintptr_t pages;
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
    pages = (end - start) / PAGE_SIZE;
    if (pages != (uint32_t)pages) {
        return -ENOMEM; /* more than a record counts */
    }
    mm_read_begin(mm);
    rc = pin_user_locked(mm, start, end, flags, holder, out);
    mm_read_end(mm);
    return rc;
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

uint32_t pin_ram_count(const struct pin *pin)
{
    return pin->ram_pages;
}

uintptr_t pin_phys(const struct pin *pin, uint32_t i)
{
    return pin->live && i < pin->count ? pin->frames[i] : 0;
}
