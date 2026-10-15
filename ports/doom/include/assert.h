/*
 * assert.h: assert for Doom on Rach2. A failed assertion prints the
 * expression, the file and the line on standard output and ends the process
 * with status 1 through _exit, so the platform is not shut down; NDEBUG
 * removes the test.
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
extern void _exit(int status);
extern int printf(const char *fmt, ...);
#define assert(expr) \
    ((expr) ? (void)0 : \
     (printf("Assertion failed: %s, file %s, line %d\n", #expr, __FILE__, __LINE__), _exit(1)))
#endif

#endif /* _ASSERT_H */
