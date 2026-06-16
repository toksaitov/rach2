#ifndef _KDEF_H_
#define _KDEF_H_

/* NULL pointer */
#define NULL ((void*) 0)

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

#endif
