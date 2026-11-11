/*
 * nmiwd.c: the registers and switch frames the lockup detectors' reports
 * read on the PC.
 */
#include "../../platform.h"

#include "../../klog.h"
#include "../../proc/process.h"
#include "../../watchdog.h"

/** Write `v` as `digits` hexadecimal digits into `out`, which takes them and
 * a terminating NUL. */
static void hex(char *out, unsigned long v, int digits)
{
    int i;

    for (i = digits - 1; i >= 0; i--) {
        out[i] = "0123456789abcdef"[v & 0xFu];
        v >>= 4;
    }
    out[digits] = '\0';
}

#if defined(__x86_64__)
#define W 16
#else
#define W 8
#endif

/**
 * Linux's x86-64 `__show_regs` (arch/x86/kernel/process_64.c:68) and i386's
 * (process_32.c) without the symbol after RIP, which this kernel cannot
 * give. A frame from kernel mode on the i686 has no stack pointer of its
 * own: the processor pushed none, and the stack resumes just above the
 * frame, as Linux's `kernel_stack_pointer` takes it.
 */
void platform_show_regs(const char *subsystem, int level, const isr_params_t *p)
{
    char a[W + 1], b[W + 1], c[W + 1];
    process_t *cur = current_proc;
    uintptr_t top = cur ? cur->kernel_stack : 0;

#if defined(__x86_64__)
    hex(a, p->rip, 16);
    klog(level, subsystem, "RIP: %04x:0x%s", (unsigned int)p->cs, a);
    hex(a, p->rsp, 16);
    hex(b, p->rflags, 8);
    klog(level, subsystem, "RSP: %04x:%s EFLAGS: %s", (unsigned int)p->ss, a, b);
    hex(a, p->rax, 16);
    hex(b, p->rbx, 16);
    hex(c, p->rcx, 16);
    klog(level, subsystem, "RAX: %s RBX: %s RCX: %s", a, b, c);
    hex(a, p->rdx, 16);
    hex(b, p->rsi, 16);
    hex(c, p->rdi, 16);
    klog(level, subsystem, "RDX: %s RSI: %s RDI: %s", a, b, c);
    hex(a, p->rbp, 16);
    hex(b, p->r8, 16);
    hex(c, p->r9, 16);
    klog(level, subsystem, "RBP: %s R08: %s R09: %s", a, b, c);
    hex(a, p->r10, 16);
    hex(b, p->r11, 16);
    hex(c, p->r12, 16);
    klog(level, subsystem, "R10: %s R11: %s R12: %s", a, b, c);
    hex(a, p->r13, 16);
    hex(b, p->r14, 16);
    hex(c, p->r15, 16);
    klog(level, subsystem, "R13: %s R14: %s R15: %s", a, b, c);
#else
    uintptr_t sp = FRAME_USER(p) ? p->user_esp : (uintptr_t)&p->user_esp;
    char d[W + 1];

    hex(a, p->eip, 8);
    klog(level, subsystem, "EIP: %04x:0x%s", p->cs & 0xFFFFu, a);
    hex(a, p->eax, 8);
    hex(b, p->ebx, 8);
    hex(c, p->ecx, 8);
    hex(d, p->edx, 8);
    klog(level, subsystem, "EAX: %s EBX: %s ECX: %s EDX: %s", a, b, c, d);
    hex(a, p->esi, 8);
    hex(b, p->edi, 8);
    hex(c, p->ebp, 8);
    hex(d, sp, 8);
    klog(level, subsystem, "ESI: %s EDI: %s EBP: %s ESP: %s", a, b, c, d);
    hex(a, p->eflags, 8);
    klog(level, subsystem, "DS: %04x ES: %04x FS: %04x GS: %04x EFLAGS: %s", p->ds & 0xFFFFu,
         p->es & 0xFFFFu, p->fs & 0xFFFFu, p->gs & 0xFFFFu, a);
#endif
    if (FRAME_USER(p)) {
        return;
    }
    watchdog_show_trace(subsystem, level, (uintptr_t)FRAME_FP(p), top);
}

/* The `cli` after the halt in `arch_idle_masked` (cpu.c). */
extern char arch_idle_resume[];

int platform_frame_idle(const isr_params_t *p)
{
    return FRAME_PC(p) == (unsigned long)arch_idle_resume;
}

void platform_show_stack(const char *subsystem, int level)
{
    process_t *cur = current_proc;
    uintptr_t fp;

#if defined(__x86_64__)
    __asm__ __volatile__("mov %%rbp, %0" : "=r"(fp));
#else
    __asm__ __volatile__("mov %%ebp, %0" : "=r"(fp));
#endif
    watchdog_show_trace(subsystem, level, fp, cur ? cur->kernel_stack : 0);
}

/** The switch pushes the callee-saved registers above its return address,
 * the frame pointer last before it: EBP three words up on the i686
 * (arch/i686/switch.s), RBP five on x86-64 (arch/x86_64/switch.s). A saved
 * stack pointer outside the thread's stack gives 0 for both. */
void platform_blocked_frame(const struct process *t, uintptr_t *pc, uintptr_t *fp)
{
#if defined(__x86_64__)
    uintptr_t at = (uintptr_t)t->regs.rsp;
    const int slot = 5;
#else
    uintptr_t at = (uintptr_t)t->regs.esp;
    const int slot = 3;
#endif
    uintptr_t top = t->kernel_stack;

    *pc = 0;
    *fp = 0;
    if (top < KERNEL_STACK_SIZE || at < top - KERNEL_STACK_SIZE ||
        at + (slot + 2) * sizeof(uintptr_t) > top || at % sizeof(uintptr_t) != 0) {
        return;
    }
    *fp = ((const uintptr_t *)at)[slot];
    *pc = ((const uintptr_t *)at)[slot + 1];
}
