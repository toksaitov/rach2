/* usercopy.c: the copies between kernel and user memory of usercopy.h. */

#include "usercopy.h"
#include "paging.h"
#include "../proc/process.h"
#include "mm.h"
#include "../arch/arch.h"

#include "../lib/kstring.h"
#include "../lib/kerrno.h"

/**
 * Bring the user range [addr, addr + len) in for the access `write` and
 * hold the address space's regions' lock to read across the access
 * (`vm_user_begin`), which keeps another thread from unmapping or
 * protecting a page between the check and the access, which the kernel
 * could not recover from. Returns -EFAULT, or 0 with `*held` saying
 * whether `user_end` gives a lock back.
 */
static int user_begin(uintptr_t addr, size_t len, int write, int *held)
{
    int rc = vm_user_begin(addr, len, write);

    *held = rc > 0;
    return rc < 0 ? -EFAULT : 0;
}

/** Give back what `user_begin` took, when `held` says it took a lock. */
static void user_end(int held)
{
    if (held) {
        vm_user_end();
    }
}

/** The length test is written as `len > USER_ADDR_MAX - addr`, so that a
 * range whose end wraps past the top of the address space is refused
 * without an overflow. */
int user_range_ok(uintptr_t addr, size_t len)
{
    if (len == 0 || proc_window_covers(current_proc, addr, len)) {
        return 1;
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        return 0;
    }
    if (len > USER_ADDR_MAX - addr) {
        return 0;
    }
    return 1;
}

int user_span_ok(uintptr_t addr, size_t len)
{
    return addr <= USER_ADDR_MAX && len <= USER_ADDR_MAX - addr;
}

void user_window_open(void *buf, uint32_t len)
{
    current_proc->uwin_base = (uintptr_t)buf;
    current_proc->uwin_len = len;
}

void user_window_close(void)
{
    current_proc->uwin_len = 0;
    current_proc->uwin_base = 0;
}

/** The range is brought in before the copy (`user_begin`), so that a page
 * not yet backed is backed there and not by a fault in the copy, and stays
 * until the copy ends. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)src;
    int held;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n) || user_begin(addr, n, 0, &held) < 0) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void __force *)src, n);
    user_end(held);
    return 0;
}

/** The range is brought in for a write before the copy, so that a page not
 * yet backed is made there and a copy-on-write page made private, and the
 * kernel's store finds each page writable, held as `copy_from_user` holds
 * its range. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)dst;
    int held;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n) || user_begin(addr, n, 1, &held) < 0) {
        return -EFAULT;
    }
    k_memcpy((void __force *)dst, src, n);
    user_end(held);
    return 0;
}

int get_user_u32(uint32_t *val, uintptr_t src)
{
    int held;

    if ((src & 3u) != 0 || !user_range_ok(src, 4) || user_begin(src, 4, 0, &held) < 0) {
        return -EFAULT;
    }
    *val = *(const volatile uint32_t *)src;
    user_end(held);
    return 0;
}

int cmpxchg_user_u32(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev)
{
    int held;

    if ((dst & 3u) != 0 || !user_range_ok(dst, 4) || user_begin(dst, 4, 1, &held) < 0) {
        return -EFAULT;
    }
    *prev = arch_atomic_cmpxchg((volatile unsigned int *)dst, old, value);
    user_end(held);
    return 0;
}

/**
 * Take the current address space's regions' lock to read for a string's
 * walk, which brings its pages in one at a time and reads each under the
 * same hold; the space, or NULL with none, for `user_strings_unlock`. A
 * string in a kernel buffer the thread opened (`user_window_open`) is read
 * under it too, which costs nothing.
 */
static struct mm *user_strings_lock(void)
{
    struct mm *mm = current_proc ? current_proc->mm : NULL;

    if (mm) {
        mmap_read_lock(mm);
    }
    return mm;
}

/** Give back the lock `user_strings_lock` took on `mm`, if any. */
static void user_strings_unlock(struct mm *mm)
{
    if (mm) {
        mmap_unlock(mm);
    }
}

/**
 * Each byte's address is checked before it is read, so a short string near
 * the top of user space is copied and an unterminated one cannot run off
 * the end. The pages are backed one at a time, not the whole `n` at once,
 * since `n` is only a cap and the page past the string's end may lie in no
 * region. Each byte is read from user memory once and tested in `dst`, so a
 * thread on another processor that stores into the string meanwhile cannot
 * make the copy end without its NUL. The caller holds the regions' lock to
 * read, once, so that a page's fill may let it go (`vm_prefault_held`).
 */
static int strncpy_locked(char *dst, const char __user *src, uint32_t n)
{
    uintptr_t addr = (__force uintptr_t)src;
    const char *s = (const char __force *)src;
    uint32_t i;

    if (n == 0) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        uintptr_t a = addr + i;

        if (!user_range_ok(a, 1)) {
            return -EFAULT;
        }
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) && !vm_prefault_held(a, 1, 0)) {
            return -EFAULT;
        }
        dst[i] = *(const volatile char *)&s[i];
        if (dst[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

/** The copy of `strncpy_from_user`, under the regions' lock
 * (`user_strings_lock`). */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    struct mm *mm = user_strings_lock();
    int r = strncpy_locked(dst, src, n);

    user_strings_unlock(mm);
    return r;
}

/** `strnlen_user` with the regions' lock held, as `strncpy_locked`. */
static int strnlen_locked(const char __user *src, uint32_t n)
{
    uintptr_t addr = (__force uintptr_t)src;
    const char *s = (const char __force *)src;
    uint32_t i;

    if (n == 0) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        uintptr_t a = addr + i;

        if (!user_range_ok(a, 1)) {
            return -EFAULT;
        }
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) && !vm_prefault_held(a, 1, 0)) {
            return -EFAULT;
        }
        if (s[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

/** The scan of `strnlen_user`, under the regions' lock
 * (`user_strings_lock`). */
int strnlen_user(const char __user *src, uint32_t n)
{
    struct mm *mm = user_strings_lock();
    int r = strnlen_locked(src, n);

    user_strings_unlock(mm);
    return r;
}
