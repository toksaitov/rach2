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
int user_range_ok(uintptr_t addr, size_t len)
{
    if (len == 0) {
        return 1; /* an empty range touches nothing */
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        return 0;
    }
    if (len > USER_ADDR_MAX - addr) {
        return 0; /* spans past the end of user space (or overflows) */
    }
    return 1;
}

/**
 * Copy `n` bytes from a user pointer into the kernel buffer `dst`.
 *
 * The range is pre-faulted before the copy, so a valid page not yet backed
 * is backed there and not by a fault in the raw copy, and a page outside
 * every VMA answers -EFAULT instead of a panic. Backing a page may take
 * frames back from the caches, which from a system call can sleep in a
 * writeback.
 *
 * Returns 0 on success and -EFAULT if any byte of the source range is outside
 * user space, in no region that may be read, or cannot be backed. Returns 0
 * for a zero-length copy.
 */
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

/**
 * Copy `n` bytes from the kernel buffer `src` into a user pointer.
 *
 * The range is pre-faulted for writing before the copy, so a lazy heap or
 * stack page is made there, a copy-on-write page is made private, and a
 * read-only or unmapped destination answers -EFAULT instead of a fault in
 * kernel mode. Backing a page may take frames back from the caches, which
 * from a system call can sleep in a writeback.
 *
 * Returns 0 on success and -EFAULT if any byte of the destination range is
 * outside user space, unmapped, read-only, or cannot be backed. Returns 0
 * for a zero-length copy.
 */
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

/**
 * Copy a NUL-terminated string from user space into `dst`, at most `n` bytes.
 *
 * Each byte's address is checked before it is read, so a short string near
 * the top of user space is valid and an unterminated one cannot walk off the
 * end. Each page is backed as the copy reaches it, not the whole `n` at once,
 * since `n` is only a cap: the string may end before a page that is
 * legitimately unmapped, and backing that page would reject a valid string.
 * Each byte is read from user memory once and tested in `dst`, so a thread
 * on another processor that stores into the string meanwhile cannot make
 * the copy end without its NUL.
 *
 * On success returns the string length not counting the NUL, which is also
 * copied. Returns -EINVAL for `n` of 0, -EFAULT if a byte lies outside user
 * space or backs no VMA, and -ENAMETOOLONG if no NUL appears within `n` bytes;
 * `dst` then has `n` bytes and no NUL.
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
    return -ENAMETOOLONG; /* no NUL within n bytes */
}

/**
 * The length of the NUL-terminated string at user address `src`, not counting
 * the NUL, when the NUL is among the first `n` bytes.
 *
 * The bytes are checked and their pages backed as `strncpy_from_user` checks
 * and backs them, one page at a time as the scan reaches it, so a string that
 * ends before an unmapped page is measured. What it measures can change
 * before the caller reads it again; a caller that copies it after bounds the
 * copy by this length.
 *
 * Returns the length; -EINVAL for `n` of 0; -EFAULT if a byte lies outside
 * user space or backs no VMA; -ENAMETOOLONG if no NUL is among the first `n`
 * bytes.
 */
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
