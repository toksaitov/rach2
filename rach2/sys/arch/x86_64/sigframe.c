/*
 * sigframe.c: the Linux x86-64 signal frame.
 *
 * x86-64 has one frame, `struct rt_sigframe`, for every handler, with or
 * without SA_SIGINFO: the address the handler returns to, the ucontext with
 * the registers in a `struct sigcontext`, and the siginfo. The
 * floating-point state, the XSAVE image or, without XSAVE, the 512-byte
 * `fxsave` image Linux calls `struct _fpstate_64` (../x86/fpu.h), lies
 * above the frame at a 64-byte boundary, and the sigcontext points at it.
 * The handler returns to `sa_restorer`, which calls rt_sigreturn: Linux
 * requires SA_RESTORER on x86-64, and musl always gives it, so the frame has
 * no return code of its own.
 */

#include "../../proc/sigframe.h"

#include "percpu.h"

#include "../x86/fpu.h"
#include "../../proc/process.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"

/* The x86-64 `struct sigcontext`, 256 bytes. */
struct lx_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate; /* the user address of the _fpstate_64, or 0 */
    uint64_t reserved1[8];
};

/* Linux's `stack_t`, as a ucontext has it. */
struct lx_stack {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

/* Linux x86-64's `struct ucontext`, with the kernel's 64-bit signal mask. */
struct lx_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack uc_stack;
    struct lx_sigcontext mc;
    uint64_t sigmask;
};

/* The frame at the handler's stack pointer: the return address its `ret`
 * takes, the ucontext and the siginfo. */
struct lx_rt_sigframe {
    uint64_t pretcode;
    struct lx_ucontext uc;
    uint8_t info[SIGINFO_SIZE];
};

#define SS_ONSTACK 1u
#define SS_DISABLE 2u

/* uc_flags: the sigcontext's `ss` is the saved one, and a return restores
 * it, as Linux 4.12 and later say. */
#define UC_SIGCONTEXT_SS     0x2u
#define UC_STRICT_RESTORE_SS 0x4u

/* The flags a frame may change: the arithmetic flags, the direction and
 * trap flags, resume and alignment check. */
#define FIX_EFLAGS 0x00050DD5u
#define EFLAGS_TF  0x00000100u
#define EFLAGS_DF  0x00000400u
#define EFLAGS_RF  0x00010000u

/* The 128 bytes below the stack pointer that the x86-64 ABI gives a
 * function for its own use without moving the pointer, which a frame must
 * not overwrite. */
#define RED_ZONE 128u

/* The lower half's end: a return address at or past it is not one `iretq`
 * can take (isr_handler.s). */
#define LOWER_HALF_END 0x0000800000000000UL

int sigframe_on_altstack(uintptr_t sp)
{
    return current_proc->altstack_on &&
           sp - (uintptr_t)current_proc->altstack_sp < (uintptr_t)current_proc->altstack_size;
}

/** Store `v` at `p`, which need not be aligned. */
static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Store `v` at `p`, which need not be aligned. */
static void put64(uint8_t *p, uint64_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** The value at `p`, which need not be aligned. */
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;

    k_memcpy(&v, p, sizeof(v));
    return v;
}

/** The value at `p`, which need not be aligned. */
static uint64_t get64(const uint8_t *p)
{
    uint64_t v;

    k_memcpy(&v, p, sizeof(v));
    return v;
}

/** Whether `signo` with `code` is a seccomp filter's SIGSYS, whose union
 * has the call's address, number and architecture. */
static int is_sigsys(int signo, int32_t code)
{
    return signo == SIGSYS && code == SIGSYS_SECCOMP;
}

/**
 * The x86-64 siginfo: signo, errno and code, then at byte 16, past the
 * padding that aligns the union, Linux x86-64's fields of the kind the
 * code says: a fault's address; a seccomp SIGSYS's call address, call
 * number at 24 and architecture at 28; and otherwise the sender's or
 * child's pid and uid at 16 and 20 and, at 24, a timer's or a queued
 * signal's si_value, all eight bytes, or a child's status. For a timer the
 * first two are its id and overrun.
 */
void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    int fault = (signo == SIGSEGV || signo == SIGBUS || signo == SIGILL || signo == SIGFPE ||
                 signo == SIGTRAP) &&
                info->code > 0;

    k_memset(out, 0, SIGINFO_SIZE);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    if (fault) {
        put64(out + 16, (uint64_t)info->addr);
        return;
    }
    if (is_sigsys(signo, info->code)) {
        put64(out + 16, (uint64_t)info->addr);
        put32(out + 24, info->uid);
        put32(out + 28, (uint32_t)info->status);
        return;
    }
    put32(out + 16, info->pid);
    put32(out + 20, info->uid);
    if (info->code == SI_TIMER || info->code == SI_QUEUE || info->code == SI_MESGQ) {
        put64(out + 24, (uint64_t)info->value);
    } else {
        put32(out + 24, (uint32_t)info->status);
    }
}

/** The fields of the x86-64 siginfo at `in` that sigframe_fill_siginfo
 * writes, for rt_sigqueueinfo and PTRACE_SETSIGINFO, read by the signal and
 * the code the siginfo has: a field another kind has there is read too,
 * and the fill of the kind the code says puts back the one it has. */
void sigframe_take_siginfo(const uint8_t *in, struct k_siginfo *info)
{
    int32_t code = (int32_t)get32(in + 8);

    k_memset(info, 0, sizeof(*info));
    info->err = (int32_t)get32(in + 4);
    info->code = code;
    info->addr = get64(in + 16);
    if (is_sigsys((int)get32(in), code)) {
        info->pid = (uint32_t)info->addr;
        info->uid = get32(in + 24);
        info->status = (int32_t)get32(in + 28);
        return;
    }
    info->pid = get32(in + 16);
    info->uid = get32(in + 20);
    info->status = (int32_t)get32(in + 24);
    info->value = get64(in + 24);
}

/**
 * Fill `sc` from the frame `tf`, with the number, error code and address of
 * the current process's last processor exception, the user address of the
 * floating-point state `fp_addr` and the mask `oldmask`. The FS and GS
 * fields are left alone: the caller's frame has 0 there, which is what
 * Linux writes.
 */
static void fill_sigcontext(struct lx_sigcontext *sc, const isr_params_t *tf, uint64_t fp_addr,
                            uint64_t oldmask)
{
    sc->r8 = tf->r8;
    sc->r9 = tf->r9;
    sc->r10 = tf->r10;
    sc->r11 = tf->r11;
    sc->r12 = tf->r12;
    sc->r13 = tf->r13;
    sc->r14 = tf->r14;
    sc->r15 = tf->r15;
    sc->rdi = tf->rdi;
    sc->rsi = tf->rsi;
    sc->rbp = tf->rbp;
    sc->rbx = tf->rbx;
    sc->rdx = tf->rdx;
    sc->rax = tf->rax;
    sc->rcx = tf->rcx;
    sc->rsp = tf->rsp;
    sc->rip = tf->rip;
    sc->eflags = tf->rflags;
    sc->cs = (uint16_t)tf->cs;
    sc->ss = (uint16_t)tf->ss;
    sc->err = current_proc->trap_err;
    sc->trapno = current_proc->trap_nr;
    sc->oldmask = oldmask;
    sc->cr2 = current_proc->trap_addr;
    sc->fpstate = fp_addr;
}

/** The frame's alignment in `sigframe_setup` costs up to 15 bytes, and the
 * 8 below the 16-byte boundary 8 more. */
uintptr_t sigframe_stack_bytes(void)
{
    return fpu_frame_bytes() + sizeof(struct lx_rt_sigframe) + 15u + 8u;
}

/**
 * Every handler gets the rt frame, with or without SA_SIGINFO, and an
 * action without SA_RESTORER is refused with -1, since the frame has no
 * return code. The frame goes below the red zone of the interrupted code,
 * or at the top of the alternate stack, with the floating-point state above
 * it (`fpu_frame_place`), and starts 8 bytes below a 16-byte boundary, as a
 * function's stack does after the call that entered it. The ucontext's
 * `ss_flags` are what sigaltstack would have read where the signal came, as
 * Linux saves them: SS_ONSTACK only when the thread was already on the
 * alternate stack.
 *
 * The handler starts with the initial floating-point state, as a new image
 * does, its arguments in RDI, RSI and RDX, and RAX 0, which a variadic
 * handler reads, in AL, as the count of its arguments in vector registers.
 * DF, TF and RF are cleared, and CS and SS are the user's.
 */
int sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                   const struct k_siginfo *info, uint64_t oldmask)
{
    struct lx_rt_sigframe frame;
    int on_alt, was_on;
    uintptr_t sp = tf->rsp - RED_ZONE;
    uintptr_t fp_addr, frame_addr;

    if (!(sa->flags & SA_RESTORER)) {
        return -1;
    }
    on_alt = was_on = sigframe_on_altstack(tf->rsp);
    if ((sa->flags & SA_ONSTACK) && current_proc->altstack_on && !on_alt) {
        sp = (uintptr_t)current_proc->altstack_sp + (uintptr_t)current_proc->altstack_size;
        on_alt = 1;
    }
    fp_addr = fpu_frame_place(sp);
    sp = fp_addr - sizeof(frame);
    sp = (sp & ~(uintptr_t)15u) - 8u;
    frame_addr = sp;
    if (on_alt && !sigframe_on_altstack(frame_addr)) {
        return -1;
    }

    fpu_save(current_proc->fpu);
    if (fpu_frame_put(fp_addr, current_proc->fpu) != 0) {
        return -1;
    }

    k_memset(&frame, 0, sizeof(frame));
    frame.pretcode = (uintptr_t)sa->restorer;
    frame.uc.uc_flags = UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS | fpu_frame_uc_flags();
    frame.uc.uc_stack.ss_sp = (uintptr_t)current_proc->altstack_sp;
    frame.uc.uc_stack.ss_size = (uintptr_t)current_proc->altstack_size;
    frame.uc.uc_stack.ss_flags =
        (int32_t)(!current_proc->altstack_on ? SS_DISABLE : (was_on ? SS_ONSTACK : 0u));
    fill_sigcontext(&frame.uc.mc, tf, fp_addr, oldmask);
    frame.uc.sigmask = oldmask;
    sigframe_fill_siginfo(frame.info, signo, info);
    if (copy_to_user((void __user *)frame_addr, &frame, sizeof(frame)) != 0) {
        return -1;
    }

    fpu_reset();
    tf->rsp = frame_addr;
    tf->rip = (uintptr_t)sa->handler;
    tf->rdi = (uint64_t)signo;
    tf->rsi = frame_addr + offsetof(struct lx_rt_sigframe, info);
    tf->rdx = frame_addr + offsetof(struct lx_rt_sigframe, uc);
    tf->rax = 0;
    tf->cs = GDT_USER_CODE;
    tf->ss = GDT_USER_DATA;
    tf->rflags &= ~(uint64_t)(EFLAGS_DF | EFLAGS_TF | EFLAGS_RF);
    return 0;
}

/**
 * The frame starts 8 bytes below the stack pointer, since the handler's
 * `ret` took `pretcode`. There is no plain sigreturn on x86-64, and a call
 * without `rt` is refused with -1. A return address outside the lower half
 * is refused too: `iretq` itself would fault on it. CS and SS are the
 * user's whatever the frame says, and a frame without floating-point state
 * gets the initial one.
 *
 * The frame's alternate stack comes back as sigaltstack would take it,
 * unless the restored stack pointer is on the alternate stack now in use; a
 * stack of under 2048 bytes is not taken.
 */
int sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask)
{
    struct lx_rt_sigframe frame;
    const struct lx_sigcontext *sc = &frame.uc.mc;
    uintptr_t frame_addr = tf->rsp - 8u;

    if (!rt) {
        return -1;
    }
    if (copy_from_user(&frame, (const void __user *)frame_addr, sizeof(frame)) != 0) {
        return -1;
    }
    if (sc->rip >= LOWER_HALF_END) {
        return -1;
    }
    *mask = frame.uc.sigmask;

    if (sc->fpstate != 0) {
        if (fpu_frame_load((uintptr_t)sc->fpstate) != 0) {
            return -1;
        }
    } else {
        fpu_reset();
    }

    tf->r8 = sc->r8;
    tf->r9 = sc->r9;
    tf->r10 = sc->r10;
    tf->r11 = sc->r11;
    tf->r12 = sc->r12;
    tf->r13 = sc->r13;
    tf->r14 = sc->r14;
    tf->r15 = sc->r15;
    tf->rdi = sc->rdi;
    tf->rsi = sc->rsi;
    tf->rbp = sc->rbp;
    tf->rbx = sc->rbx;
    tf->rdx = sc->rdx;
    tf->rax = sc->rax;
    tf->rcx = sc->rcx;
    tf->rsp = sc->rsp;
    tf->rip = sc->rip;
    tf->cs = GDT_USER_CODE;
    tf->ss = GDT_USER_DATA;
    tf->rflags = (tf->rflags & ~(uint64_t)FIX_EFLAGS) | (sc->eflags & FIX_EFLAGS);

    if (!sigframe_on_altstack(tf->rsp)) {
        if (frame.uc.uc_stack.ss_flags & SS_DISABLE) {
            current_proc->altstack_on = 0;
        } else if (frame.uc.uc_stack.ss_size >= 2048u) {
            current_proc->altstack_sp = frame.uc.uc_stack.ss_sp;
            current_proc->altstack_size = frame.uc.uc_stack.ss_size;
            current_proc->altstack_on = 1;
        }
    }
    return 0;
}
