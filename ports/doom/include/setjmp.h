/*
 * setjmp.h: stubs of setjmp and longjmp for Doom on Rach2, which the engine
 * does not call.
 */
#ifndef _SETJMP_H
#define _SETJMP_H

/* Room for the six registers an i386 setjmp saves, ebx, esi, edi, ebp, esp
 * and eip; this one saves nothing in it. */
typedef int jmp_buf[6];

/** A stub: saves nothing and returns 0. */
static inline int setjmp(jmp_buf env) {
    (void)env;
    return 0;
}

/** A stub: ends the process with status 1 through _exit. */
static inline void longjmp(jmp_buf env, int val) {
    (void)env; (void)val;
    extern void _exit(int status);
    _exit(1);
}

#endif /* _SETJMP_H */
