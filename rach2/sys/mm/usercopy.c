/* usercopy.c: the copies between kernel and user memory of usercopy.h. */

#include "usercopy.h"
#include "paging.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../arch/arch.h"

#include "../lib/kstring.h"
#include "../lib/kerrno.h"

/**
 * Take the big lock for a copy made without it, and return nonzero when this
 * took it, for `user_unlock`. Until an address space has a lock of its own,
 * the big lock is what keeps another thread from unmapping or protecting a
 * page between the copy's check of the range and its access, which the
 * kernel could not recover from; a call that runs without the lock takes it
 * for each copy.
 */
static int user_lock(void)
{
    if (bkl_held()) {
        return 0;
    }
    bkl_enter();
    return 1;
}

/** Give back the lock `user_lock` took, when `taken` says it did. */
static void user_unlock(int taken)
{
    if (taken) {
        bkl_exit();
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

/** The range is brought in before the copy (`vm_prefault_range`), so that
 * a page not yet backed is backed there and not by a fault in the copy, both
 * under the big lock (`user_lock`). */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)src;
    int taken;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n)) {
        return -EFAULT;
    }
    taken = user_lock();
    if (!vm_prefault_range(addr, n, 0)) {
        user_unlock(taken);
        return -EFAULT;
    }
    k_memcpy(dst, (const void __force *)src, n);
    user_unlock(taken);
    return 0;
}

/** The range is brought in for a write before the copy, so that a page not
 * yet backed is made there and a copy-on-write page made private, and the
 * kernel's store finds each page writable, under the big lock as
 * `copy_from_user` reads. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)dst;
    int taken;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n)) {
        return -EFAULT;
    }
    taken = user_lock();
    if (!vm_prefault_range(addr, n, 1)) {
        user_unlock(taken);
        return -EFAULT;
    }
    k_memcpy((void __force *)dst, src, n);
    user_unlock(taken);
    return 0;
}

int get_user_u32(uint32_t *val, uintptr_t src)
{
    int taken;

    if ((src & 3u) != 0 || !user_range_ok(src, 4)) {
        return -EFAULT;
    }
    taken = user_lock();
    if (!vm_prefault_range(src, 4, 0)) {
        user_unlock(taken);
        return -EFAULT;
    }
    *val = *(const volatile uint32_t *)src;
    user_unlock(taken);
    return 0;
}

int cmpxchg_user_u32(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev)
{
    int taken;

    if ((dst & 3u) != 0 || !user_range_ok(dst, 4)) {
        return -EFAULT;
    }
    taken = user_lock();
    if (!vm_prefault_range(dst, 4, 1)) {
        user_unlock(taken);
        return -EFAULT;
    }
    *prev = arch_atomic_cmpxchg((volatile unsigned int *)dst, old, value);
    user_unlock(taken);
    return 0;
}

/**
 * Each byte's address is checked before it is read, so a short string near
 * the top of user space is copied and an unterminated one cannot run off
 * the end. The pages are backed one at a time, not the whole `n` at once,
 * since `n` is only a cap and the page past the string's end may lie in no
 * region. Each byte is read from user memory once and tested in `dst`, so a
 * thread on another processor that stores into the string meanwhile cannot
 * make the copy end without its NUL. The caller holds the big lock.
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
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) && !vm_prefault_range(a, 1, 0)) {
            return -EFAULT;
        }
        dst[i] = *(const volatile char *)&s[i];
        if (dst[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

/** The copy of `strncpy_from_user`, under the big lock (`user_lock`). */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    int taken = user_lock();
    int r = strncpy_locked(dst, src, n);

    user_unlock(taken);
    return r;
}

/** `strnlen_user` with the big lock held. */
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
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) && !vm_prefault_range(a, 1, 0)) {
            return -EFAULT;
        }
        if (s[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

/** The scan of `strnlen_user`, under the big lock (`user_lock`). */
int strnlen_user(const char __user *src, uint32_t n)
{
    int taken = user_lock();
    int r = strnlen_locked(src, n);

    user_unlock(taken);
    return r;
}
