/*
 * usertest.c: the debug shell's ring-3 exercise on an i686, a payload that
 * writes a message through `int 0x80` and spins.
 */

#include "../arch.h"

/* Ring-3 test message; must live in user-accessible memory. */
const char arch_user_test_msg[] = "Hello from ring 3!\n";

/**
 * Ring-3 test payload: write the test message, then spin forever.
 *
 * Runs in user mode, so it cannot halt; after the write syscall returns it
 * loops on nop. arch_enter_user jumps here; it never returns.
 */
void arch_user_test_entry(void)
{
    /* write(1, arch_user_test_msg, 19) via int 0x80. */
    __asm__ __volatile__("movl $4, %%eax\n\t"
                         "movl $1, %%ebx\n\t"
                         "movl %0, %%ecx\n\t"
                         "movl $19, %%edx\n\t"
                         "int $0x80\n\t"
                         :
                         : "r"(arch_user_test_msg)
                         : "eax", "ebx", "ecx", "edx", "memory");

    for (;;) {
        __asm__ __volatile__("nop");
    }
}
