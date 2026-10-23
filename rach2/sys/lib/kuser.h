#ifndef _KUSER_H_
#define _KUSER_H_

/*
 * Address-space annotations for user pointers, in the style of the Linux
 * kernel's __user. In a normal build they expand to nothing. Under sparse
 * (which defines __CHECKER__) a __user pointer is tagged noderef and a distinct
 * address space, so dereferencing one directly, instead of going through the
 * copy_from_user/copy_to_user helpers, becomes a sparse error. __force marks
 * intentional removal of the annotation: copies, validated direct-I/O buffers
 * and conversions to integer addresses for range checks and page accounting.
 * RACH2_USERCHECK uses Clang's equivalent pointer attributes and a parsed
 * cast audit during preparation; the ordinary build is unchanged.
 */
#if defined(RACH2_USERCHECK) && defined(__clang__)
#define __user   __attribute__((noderef, address_space(1)))
#define __kernel __attribute__((address_space(0)))
/* Clang's explicit casts are checked by tools/verify/usercheck.py. */
#define __force
#elif defined(__CHECKER__)
#define __user   __attribute__((noderef, address_space(1)))
#define __kernel __attribute__((address_space(0)))
#define __force  __attribute__((force))
#else
#define __user
#define __kernel
#define __force
#endif

#endif /* _KUSER_H_ */
