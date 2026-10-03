#ifndef _KINT_H_
#define _KINT_H_

/* Signed 8-bit byte. */
typedef signed char int8_t;
/* Unsigned 8-bit byte. */
typedef unsigned char uint8_t;

/* Signed 16-bit word. */
typedef signed short int16_t;
/* Unsigned 16-bit word. */
typedef unsigned short uint16_t;

/* Signed 32-bit double word. */
typedef signed int int32_t;
/* Unsigned 32-bit double word. */
typedef unsigned int uint32_t;

/* Signed 64-bit quad word. */
typedef signed long long int64_t;
/* Unsigned 64-bit quad word. */
typedef unsigned long long uint64_t;

/*
 * An integer as wide as a pointer: 32 bits on i686 and 32-bit ARM, 64 on
 * x86-64 and 64-bit ARM. The compiler's own name for the type is taken,
 * so the type is the one its builtins and its warnings expect; TinyCC,
 * which does not define it, gets `long`, as wide as a pointer on all four.
 */
#ifdef __UINTPTR_TYPE__
typedef __UINTPTR_TYPE__ uintptr_t;
#else
typedef unsigned long uintptr_t;
#endif
#ifdef __INTPTR_TYPE__
typedef __INTPTR_TYPE__ intptr_t;
#else
typedef long intptr_t;
#endif

#endif
