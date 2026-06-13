#include "usercopy.h"

#include "../lib/kstring.h"
#include "../lib/kerrno.h"

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
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

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
    k_memcpy((void __force *)dst, src, n);
    return 0;
}

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
        /* Validate each byte's address before reading it -- a short string near
         * the top of user space stays valid while an unterminated one cannot
         * walk off the end. */
        if (!user_range_ok(addr + i, 1))
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
