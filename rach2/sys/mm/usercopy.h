#ifndef _USERCOPY_H_
#define _USERCOPY_H_

#include "../arch/arch.h"
#include "../lib/kint.h"
#include "../lib/kuser.h"

/*
 * The copy-from/to-user contract. These helpers range-check a user pointer,
 * fault in any not-yet-present user page, and copy, so that a kernel touch of
 * an unmapped user address becomes -EFAULT in one place, not a panic
 * from a raw dereference scattered across every syscall. User pointers are
 * annotated __user (see lib/kuser.h) so a sparse pass proves every access goes
 * through these helpers.
 *
 * Linux and BSD analogue: copy_from_user, copy_to_user, strncpy_from_user.
 */

/* The user address range is the architecture's, USER_ADDR_MIN and
 * USER_ADDR_MAX (arch/arch.h). */

/* A user's `size_t` length as the 32 bits the kernel's buffers count in,
 * a length past 4 GiB taken as the most that fits. */
#define USER_LEN32(n) ((n) == (uint32_t)(n) ? (uint32_t)(n) : 0xFFFFFFFFu)

/* The count of one read or write, at most Linux's MAX_RW_COUNT, the largest
 * int rounded down to a page, so that the bytes it moves fit the signed
 * 32-bit result the calls return: a larger count moves that many, as on
 * Linux, and never reads as an error. */
#define USER_RW_MAX    0x7FFFF000u
#define USER_RW_LEN(n) ((n) > USER_RW_MAX ? USER_RW_MAX : (uint32_t)(n))

/* Whether `v`, a result that is either a user address or a negative errno,
 * is the errno: one of the last 4095 values, as Linux's IS_ERR_VALUE has it.
 * A sign test is wrong, since on i686 an address above 2 GiB is negative as
 * a long. */
#define USER_ADDR_ERRNO(v) ((unsigned long)(v) > (unsigned long)-4096L)

/** Report whether [addr, addr+len) lies wholly within user space; 1 yes, 0
 * no. */
int user_range_ok(uintptr_t addr, size_t len);

/** Copy n bytes from a user pointer into kernel memory; 0 ok, -EFAULT on a
 * bad range. */
int copy_from_user(void *dst, const void __user *src, size_t n);

/** Copy n bytes from kernel memory to a user pointer; 0 ok, -EFAULT on a
 * bad range. */
int copy_to_user(void __user *dst, const void *src, size_t n);

/**
 * Read the aligned 32-bit user word at `src` into `*val` in one load, which a
 * store from another processor cannot tear, after the checks and the backing
 * `copy_from_user` makes; 0, or -EFAULT for an address that is not aligned
 * or that `copy_from_user` refuses.
 */
int get_user_u32(uint32_t *val, uintptr_t src);

/**
 * Store `value` in the aligned 32-bit user word at `dst` if it holds `old`,
 * in one compare-and-exchange no other processor's access comes between,
 * after the checks and the backing `copy_to_user` makes; what the word held
 * goes to `*prev`. 0, or -EFAULT for an address that is not aligned or that
 * `copy_to_user` refuses.
 */
int cmpxchg_user_u32(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev);

/** Copy a NUL-terminated string from user space into dst, at most n bytes;
 * returns its length, or a negative errno. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n);

/** The length of the NUL-terminated user string at src, the NUL not
 * counted, when its NUL lies within n bytes; -ENAMETOOLONG when it does not,
 * -EFAULT for an address the caller may not read, and -EINVAL for an n of
 * 0. */
int strnlen_user(const char __user *src, uint32_t n);

#endif /* _USERCOPY_H_ */
