/*
 * C runtime startup for ish: the `_start` entry, with no libc to set up the
 * stack first.
 */

#include "ish_syscalls.h"

int main(int argc, char **argv, char **envp);

#if defined __x86_64__
/**
 * Freestanding ELF entry for x86-64: read `argc`, `argv`, and `envp` off the
 * initial stack, run `main`, and exit with its return value.
 *
 * The stack `execve` hands a new process has the layout the i386 entry below
 * describes, in 64-bit words: `argc` at RSP, `argv` at RSP+8, and `envp`
 * after the NULL that ends `argv`, at RSP+8*(argc+2). The entry is assembly
 * because a C function's prologue moves RSP before any statement could read
 * it, and because the ABI enters `_start` with RSP aligned to 16 bytes, where
 * a called function expects it 8 bytes past that; the `call` to `main`
 * restores the alignment `main` expects. RBP is cleared to mark the outermost
 * frame. The arguments go to `main` in RDI, RSI and RDX, and its status to
 * `ish_exit` in EDI.
 */
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n\t"
        "xorq %rbp, %rbp\n\t"
        "movq (%rsp), %rdi\n\t"
        "leaq 8(%rsp), %rsi\n\t"
        "leaq 16(%rsp,%rdi,8), %rdx\n\t"
        "andq $-16, %rsp\n\t"
        "call main\n\t"
        "movl %eax, %edi\n\t"
        "call ish_exit\n\t"
        "hlt\n");
#else
/**
 * Freestanding ELF entry: read `argc`, `argv`, and `envp` off the initial
 * stack, run `main`, and exit with its return value.
 *
 * The kernel's `execve` hands a new process a stack whose top words are
 * `argc`, then the `argc` pointers of `argv`, a NULL terminator, the `envp`
 * pointers, and a final NULL. There is no libc startup, so this entry reads
 * that layout directly. The compiler's prologue (`push %ebp; mov %esp,%ebp`)
 * has already moved ESP, so the words are addressed EBP-relative: `argc` at
 * `EBP+4`, just above the saved EBP, and `argv` at `EBP+8`. `envp` follows
 * `argv` past its NULL terminator, at `argv + argc + 1`.
 *
 * This needs the frame pointer. Every build of ish keeps it: the host builds
 * compile without optimization and TinyCC always sets one up. A build that
 * omitted it would read the wrong words.
 */
void _start(void)
{
    int argc;
    char **argv;
    char **envp;
    int ret;

    __asm__ __volatile__("movl 4(%%ebp), %0" : "=r"(argc));

    __asm__ __volatile__("leal 8(%%ebp), %0" : "=r"(argv));

    envp = argv + argc + 1;

    ret = main(argc, argv, envp);

    ish_exit(ret);
}
#endif
