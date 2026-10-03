/*
 * usertest.c: the debug shell's ring-3 exercise on x86-64, a payload that
 * writes a message through `syscall` and spins.
 */

#include "../arch.h"

/* Ring-3 test message; must live in user-accessible memory. */
const char arch_user_test_msg[] = "Hello from ring 3!\n";

/**
 * Ring-3 test payload: write the test message, then spin forever.
 *
 * write(1, arch_user_test_msg, 19) is Linux x86-64's call 1, with its
 * arguments in RDI, RSI and RDX; `syscall` takes RCX and R11 for the
 * return. arch_enter_user jumps here; it never returns.
 */
void arch_user_test_entry(void)
{
    __asm__ __volatile__("movl $1, %%eax\n\t"
                         "movl $1, %%edi\n\t"
                         "movq %0, %%rsi\n\t"
                         "movl $19, %%edx\n\t"
                         "syscall\n\t"
                         :
                         : "r"(arch_user_test_msg)
                         : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory");

    for (;;) {
        __asm__ __volatile__("nop");
    }
}
