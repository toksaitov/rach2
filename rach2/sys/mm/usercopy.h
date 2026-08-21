#ifndef _USERCOPY_H_
#define _USERCOPY_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"

/*
 * The copy-from/to-user contract. These helpers range-check a user pointer,
 * fault in any not-yet-present user page, and copy, so that a kernel touch of
 * an unmapped user address becomes -EFAULT in one place rather than a panic
 * from a raw dereference scattered across every syscall. User pointers are
 * annotated __user (see lib/kuser.h) so a sparse pass proves every access goes
 * through these helpers.
 *
 * Linux and BSD analogue: copy_from_user, copy_to_user, strncpy_from_user.
 */

/*
 * User virtual address range. Must match the kernel's user mapping (the loader
 * places images and the stack within [USER_ADDR_MIN, USER_ADDR_MAX)).
 */
#define USER_ADDR_MIN 0x02000000UL
#define USER_ADDR_MAX 0xC0000000UL

/** Report whether [addr, addr+len) lies wholly within user space; 1 yes, 0
 * no. */
int user_range_ok(uint32_t addr, uint32_t len);

/** Copy n bytes from a user pointer into kernel memory; 0 ok, -EFAULT on a
 * bad range. */
int copy_from_user(void *dst, const void __user *src, uint32_t n);

/** Copy n bytes from kernel memory to a user pointer; 0 ok, -EFAULT on a
 * bad range. */
int copy_to_user(void __user *dst, const void *src, uint32_t n);

/** Copy a NUL-terminated string from user space into dst, at most n bytes;
 * returns its length, or a negative errno. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n);

#endif /* _USERCOPY_H_ */
