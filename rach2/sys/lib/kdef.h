#ifndef _KDEF_H_
#define _KDEF_H_

/* NULL pointer */
#define NULL ((void *)0)

/* Boolean data type */
typedef int bool;

#define false 0
#define true  1

/* Unsigned size of an object, in bytes. */
typedef unsigned int size_t;

/* Signed size, for a byte count or a negative error return. */
typedef signed int ssize_t;

/* Signed difference between two pointers. */
typedef signed int ptrdiff_t;

/*
 * What the compiler is told about a function beyond its type. `K_NORETURN`
 * marks one that never returns, so an analyser stops at a panic instead of
 * following the path after it; `K_MALLOC(free)` marks an allocator whose
 * result is released by `free`, so the analyser can follow a block from its
 * allocation to its release or to a leak. GCC knows both; TinyCC and clang
 * are given only the first, and a host test, whose panic stub returns to
 * the test, is given neither.
 */
#if defined(RACH2_HOSTTEST)
#define K_NORETURN
#define K_MALLOC(free)
#elif defined(__GNUC__) && !defined(__clang__) && !defined(__TINYC__) && __GNUC__ >= 11
#define K_NORETURN     __attribute__((noreturn))
#define K_MALLOC(free) __attribute__((malloc, malloc(free)))
#else
#define K_NORETURN __attribute__((noreturn))
#define K_MALLOC(free)
#endif

#endif
