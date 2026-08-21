#include "usercopy.h"
#include "paging.h"

#include "../lib/kstring.h"
#include "../lib/kerrno.h"

/**
 * Report whether [addr, addr+len) lies wholly within user space.
 *
 * The length test is written as `len > USER_ADDR_MAX - addr` so a range whose
 * end wraps past the top of the address space is rejected without overflowing.
 * An empty range is always in bounds.
 */
int user_range_ok(uint32_t addr, uint32_t len)
{
    if (len == 0)
    {
        return 1;  /* an empty range touches nothing */
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX)
    {
        return 0;
    }
    if (len > USER_ADDR_MAX - addr)
    {
        return 0;  /* spans past the end of user space (or overflows) */
    }
    return 1;
}

/**
 * Copy `n` bytes from a user pointer into the kernel buffer `dst`.
 *
 * Returns 0 on success and -EFAULT if any byte of the source range is outside
 * user space or backs no VMA. Returns 0 for a zero-length copy.
 */
int copy_from_user(void *dst, const void __user *src, uint32_t n)
{
    uint32_t addr = (uint32_t)(__force uintptr_t)src;

    if (n == 0)
    {
        return 0;
    }
    if (!user_range_ok(addr, n))
    {
        return -EFAULT;
    }
    /* Back any demand-paged pages in the range before the kernel reads them, so
     * a not-yet-present (but valid) user page faults in here rather than from
     * the raw k_memcpy below; a page outside every VMA returns -EFAULT, not a
     * panic. */
    if (!vm_prefault_range(addr, n, 0))
    {
        return -EFAULT;
    }
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

/**
 * Copy `n` bytes from the kernel buffer `src` into a user pointer.
 *
 * Returns 0 on success and -EFAULT if any byte of the destination range is
 * outside user space, unmapped, or read-only. Returns 0 for a zero-length copy.
 */
int copy_to_user(void __user *dst, const void *src, uint32_t n)
{
    uint32_t addr = (uint32_t)(__force uintptr_t)dst;

    if (n == 0)
    {
        return 0;
    }
    if (!user_range_ok(addr, n))
    {
        return -EFAULT;
    }
    /* Writing into user space: fault the pages in with write permission, so a
     * lazily-allocated heap/stack page is created here and a read-only or
     * unmapped destination yields -EFAULT instead of faulting in kernel mode.
     */
    if (!vm_prefault_range(addr, n, 1))
    {
        return -EFAULT;
    }
    k_memcpy((void __force *)dst, src, n);
    return 0;
}

/**
 * Copy a NUL-terminated string from user space into `dst`, at most `n` bytes.
 *
 * On success returns the string length not counting the NUL, which is also
 * copied. Returns -EINVAL for `n` of 0, -EFAULT if a byte lies outside user
 * space or backs no VMA, and -ENAMETOOLONG if no NUL appears within `n` bytes.
 */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uint32_t addr = (uint32_t)(__force uintptr_t)src;
    const char *s = (const char __force *)src;
    uint32_t i;

    if (n == 0)
    {
        return -EINVAL;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t a = addr + i;

        /* Validate each byte's address before reading it. A short string near
         * the top of user space stays valid while an unterminated one cannot
         * walk off the end. */
        if (!user_range_ok(a, 1))
        {
            return -EFAULT;
        }
        /* Back the page before the read, once per page as we cross each
         * boundary. The range is not prefaulted whole because n is only a cap:
         * the string may terminate before a later (legitimately unmapped) page,
         * and faulting that page in would wrongly reject a valid short string.
         */
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) && !vm_prefault_range(a, 1, 0))
        {
            return -EFAULT;
        }
        dst[i] = s[i];
        if (s[i] == '\0')
        {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;  /* no NUL within n bytes */
}
