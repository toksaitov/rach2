/*
 * The copies between kernel and user memory. These helpers check a user
 * range, back every page of it that is not yet present, and copy, so that a
 * kernel touch of a bad user address becomes -EFAULT in one place and not a
 * panic in whichever system call dereferenced it. User pointers are marked
 * __user (lib/kuser.h), so that sparse reports one dereferenced outside
 * these helpers. Linux has the same names; BSD has copyin, copyout and
 * copyinstr.
 *
 * The copies back pages as a fault would, for the current process, and so
 * may sleep: taking frames back from the caches can wait on a writeback.
 * With no current process, or one without an address space, every non-empty
 * copy is -EFAULT.
 */
#ifndef _USERCOPY_H_
#define _USERCOPY_H_

#include "../arch/arch.h"
#include "../lib/kint.h"
#include "../lib/kuser.h"

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

/** 1 if [addr, addr + len) lies wholly within user space, USER_ADDR_MIN to
 * USER_ADDR_MAX (arch/arch.h), and does not wrap; else 0. An empty range is
 * always 1. */
int user_range_ok(uintptr_t addr, size_t len);

/** Copy `n` bytes from the user address `src` into `dst`: 0, also for an
 * `n` of 0, or -EFAULT when a byte of the source is outside user space, in
 * no region that may be read, or cannot be backed. */
int copy_from_user(void *dst, const void __user *src, size_t n);

/** Copy `n` bytes from `src` to the user address `dst`, after making each
 * copy-on-write page of it private: 0, also for an `n` of 0, or -EFAULT when
 * a byte of the destination is outside user space, in no region that may be
 * written, or cannot be backed. */
int copy_to_user(void __user *dst, const void *src, size_t n);

/**
 * Read the aligned 32-bit user word at `src` into `*val` in one load, which a
 * store from another processor cannot tear, after the checks and the backing
 * `copy_from_user` makes; 0, or -EFAULT for an address that is not aligned
 * or that `copy_from_user` would answer with -EFAULT.
 */
int get_user_u32(uint32_t *val, uintptr_t src);

/**
 * Store `value` in the aligned 32-bit user word at `dst` if it holds `old`,
 * in one compare-and-exchange no other processor's access comes between,
 * after the checks and the backing `copy_to_user` makes; what the word held
 * goes to `*prev`. 0, or -EFAULT for an address that is not aligned or that
 * `copy_to_user` would answer with -EFAULT.
 */
int cmpxchg_user_u32(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev);

/**
 * Copy the NUL-terminated string at the user address `src` into `dst`, at
 * most `n` bytes. Each page is backed as the copy reaches it, so a string
 * that ends before a page outside every region is copied. Returns the
 * length, the NUL not counted but copied; -EINVAL for an `n` of 0; -EFAULT
 * when a byte lies outside user space or cannot be read; -ENAMETOOLONG when
 * no NUL is among the first `n` bytes, and `dst` then has `n` bytes and no
 * NUL.
 */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n);

/**
 * The length of the NUL-terminated string at the user address `src`, the
 * NUL not counted, when its NUL is among the first `n` bytes; the pages are
 * backed as `strncpy_from_user` backs them. -ENAMETOOLONG when the NUL is
 * not there, -EFAULT for an address the caller may not read, and -EINVAL
 * for an `n` of 0. The string can change before the caller reads it again,
 * so a copy after bounds itself by this length.
 */
int strnlen_user(const char __user *src, uint32_t n);

#endif /* _USERCOPY_H_ */
