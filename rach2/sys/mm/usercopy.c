/* usercopy.c: the copies between kernel and user memory of usercopy.h. */

#include "usercopy.h"
#include "paging.h"
#include "../arch/arch.h"

#include "../lib/kstring.h"
#include "../lib/kerrno.h"

/** The length test is written as `len > USER_ADDR_MAX - addr`, so that a
 * range whose end wraps past the top of the address space is refused
 * without an overflow. */
int user_range_ok(uintptr_t addr, size_t len)
{
    if (len == 0) {
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

/** The range is brought in before the copy (`vm_prefault_range`), so that
 * a page not yet backed is backed there and not by a fault in the copy. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)src;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n)) {
        return -EFAULT;
    }
    if (!vm_prefault_range(addr, n, 0)) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

/** The range is brought in for a write before the copy, so that a page not
 * yet backed is made there and a copy-on-write page made private, and the
 * kernel's store finds each page writable. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uintptr_t addr = (__force uintptr_t)dst;

    if (n == 0) {
        return 0;
    }
    if (!user_range_ok(addr, n)) {
        return -EFAULT;
    }
    if (!vm_prefault_range(addr, n, 1)) {
        return -EFAULT;
    }
    k_memcpy((void __force *)dst, src, n);
    return 0;
}

int get_user_u32(uint32_t *val, uintptr_t src)
{
    if ((src & 3u) != 0 || !user_range_ok(src, 4) || !vm_prefault_range(src, 4, 0)) {
        return -EFAULT;
    }
    *val = *(const volatile uint32_t *)src;
    return 0;
}

int cmpxchg_user_u32(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev)
{
    if ((dst & 3u) != 0 || !user_range_ok(dst, 4) || !vm_prefault_range(dst, 4, 1)) {
        return -EFAULT;
    }
    *prev = arch_atomic_cmpxchg((volatile unsigned int *)dst, old, value);
    return 0;
}

/**
 * Each byte's address is checked before it is read, so a short string near
 * the top of user space is copied and an unterminated one cannot run off
 * the end. The pages are backed one at a time, not the whole `n` at once,
 * since `n` is only a cap and the page past the string's end may lie in no
 * region. Each byte is read from user memory once and tested in `dst`, so a
 * thread on another processor that stores into the string meanwhile cannot
 * make the copy end without its NUL.
 */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
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

int strnlen_user(const char __user *src, uint32_t n)
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
