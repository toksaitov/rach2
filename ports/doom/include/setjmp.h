/*
 * setjmp.h - Non-local jumps for Doom on Rach2
 * Note: These are stubs - setjmp/longjmp not fully supported
 */
#ifndef _SETJMP_H
#define _SETJMP_H

typedef int jmp_buf[6];  /* Save registers: ebx, esi, edi, ebp, esp, eip */

static inline int setjmp(jmp_buf env) {
    (void)env;
    return 0;
}

static inline void longjmp(jmp_buf env, int val) {
    (void)env; (void)val;
    /* Cannot actually longjmp - just exit */
    extern void _exit(int status);
    _exit(1);
}

#endif /* _SETJMP_H */
