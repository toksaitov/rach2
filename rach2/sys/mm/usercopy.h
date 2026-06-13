#ifndef _USERCOPY_H_
#define _USERCOPY_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"

/*
 * The copy-from/to-user contract. Today every user page of a running process is
 * already present, so these helpers only range-check the user pointer and copy.
 * They exist now so that when Phase 4 makes user memory demand-paged, a kernel
 * touch of a not-yet-present user page can be turned into "-EFAULT" here (or
 * faulted in) in ONE place, instead of panicking from a raw deref scattered
 * across every syscall. User pointers are annotated __user (see lib/kuser.h) so
 * a one-shot sparse pass proves every access goes through these helpers.
 *
 * Linux/BSD analogue: copy_from_user / copy_to_user / strncpy_from_user.
 */

/*
 * User virtual address range. Must match the kernel's user mapping (the loader
 * places images and the stack within [USER_ADDR_MIN, USER_ADDR_MAX)).
 */
#define USER_ADDR_MIN 0x02000000UL
#define USER_ADDR_MAX 0xC0000000UL

/*
 * Returns 1 if the whole buffer [addr, addr+len) lies inside user space (and
 * len does not overflow past the top), else 0. A len of 0 is trivially ok.
 */
int user_range_ok(uint32_t addr, uint32_t len);

/*
 * Copy n bytes from a user pointer into kernel memory. Returns 0 on success or
 * -EFAULT if the user range is invalid.
 */
int copy_from_user(void *dst, const void __user *src, uint32_t n);

/*
 * Copy n bytes from kernel memory to a user pointer. Returns 0 or -EFAULT.
 */
int copy_to_user(void __user *dst, const void *src, uint32_t n);

/*
 * Copy a NUL-terminated string from user space into dst, reading at most n
 * bytes (including the NUL). Returns the string length (excluding the NUL) on
 * success, -EFAULT on a bad user address, -ENAMETOOLONG if no NUL is found
 * within n bytes, or -EINVAL if n is 0 (no room even for the terminator).
 */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n);

#endif /* _USERCOPY_H_ */
