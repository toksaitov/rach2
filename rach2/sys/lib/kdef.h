#ifndef _KDEF_H_
#define _KDEF_H_

/* NULL pointer */
#define NULL ((void *)0)

/*
 * A compiler barrier: the compiler keeps no value of memory in a register
 * across it and moves no access over it. It names no instruction, which is
 * why it is the one assembly statement the shared code may hold.
 */
#define k_barrier() __asm__ __volatile__("" : : : "memory")

/* Boolean data type */
typedef int bool;

#define false 0
#define true  1

/*
 * The size of an object, the signed size a count or a negative error
 * return takes, and the difference between two pointers: each as wide as a
 * pointer, in the compiler's own type for it, so `sizeof` and the builtins
 * agree with the declarations here.
 */
#ifdef __SIZE_TYPE__
typedef __SIZE_TYPE__ size_t;
#else
typedef unsigned long size_t;
#endif
#ifdef __PTRDIFF_TYPE__
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#else
typedef long ssize_t;
typedef long ptrdiff_t;
#endif

/*
 * What the compiler is told about a function beyond its type. `K_NORETURN`
 * marks one that never returns, so an analyser stops at a panic instead of
 * following the path after it; `K_MALLOC(free)` marks an allocator whose
 * result is released by `free`, so the analyser can follow a block from its
 * allocation to its release or to a leak. GCC knows both; TinyCC and clang
 * are given only the first, and a host test, whose panic stub returns to
 * the test, is given neither.
 *
 * `K_NOINLINE` keeps a function out of its caller's frame. It is for a helper
 * with a sector buffer of its own: at -O2 the compiler puts several such
 * buffers in one frame, and the kernel's stack is 16 KiB with a 4 KiB limit
 * on a single frame. TinyCC does not know the attribute and does not inline
 * across functions either, so it is given nothing.
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

#if defined(__TINYC__)
#define K_NOINLINE
#else
#define K_NOINLINE __attribute__((noinline))
#endif

#endif
