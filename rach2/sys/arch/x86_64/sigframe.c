/*
 * sigframe.c: the Linux x86-64 signal frame.
 *
 * x86-64 has one frame, `struct rt_sigframe`, for every handler, with or
 * without SA_SIGINFO: the address the handler returns to, the ucontext with
 * the registers in a `struct sigcontext`, and the siginfo. The
 * floating-point state, the 512-byte `fxsave` image Linux calls `struct
 * _fpstate_64`, lies above the frame at a 64-byte boundary, and the
 * sigcontext points at it. The handler returns to `sa_restorer`, which
 * calls rt_sigreturn: Linux requires SA_RESTORER on x86-64, and musl always
 * gives it, so the frame has no return code of its own.
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

struct lx_stack {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

struct lx_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack uc_stack;
    struct lx_sigcontext mc;
    uint64_t sigmask;
};

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

static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

static void put64(uint8_t *p, uint64_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/**
 * The x86-64 siginfo: signo, errno and code, then at byte 16, past the
 * padding that aligns the union, the fault's address, or the sender's pid
 * and uid and a child's status.
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
    put32(out + 16, info->pid);
    put32(out + 20, info->uid);
    put32(out + 24, (uint32_t)info->status);
}

/** The fields of the x86-64 siginfo at `in` that sigframe_fill_siginfo
 * writes, for PTRACE_SETSIGINFO. */
void sigframe_take_siginfo(const uint8_t *in, struct k_siginfo *info)
{
    int32_t code;
    uint64_t addr;

    k_memset(info, 0, sizeof(*info));
    k_memcpy(&code, in + 8, 4);
    info->code = code;
    k_memcpy(&info->pid, in + 16, 4);
    k_memcpy(&info->uid, in + 20, 4);
    k_memcpy(&info->status, in + 24, 4);
    k_memcpy(&addr, in + 16, 8);
    info->addr = addr;
}

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

int sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                   const struct k_siginfo *info, uint64_t oldmask)
{
    struct lx_rt_sigframe frame;
    uint8_t area[FPU_STATE_SIZE];
    uint8_t fpstate[FPU_LINUX_SIZE];
    int on_alt;
    uintptr_t sp = tf->rsp - RED_ZONE;
    uintptr_t fp_addr, frame_addr;

    if (!(sa->flags & SA_RESTORER)) {
        return -1;
    }
    on_alt = sigframe_on_altstack(tf->rsp);
    if ((sa->flags & SA_ONSTACK) && current_proc->altstack_on && !on_alt) {
        sp = (uintptr_t)current_proc->altstack_sp + (uintptr_t)current_proc->altstack_size;
        on_alt = 1;
    }
    sp -= FPU_LINUX_SIZE;
    sp &= ~(uintptr_t)63u;
    fp_addr = sp;
    sp -= sizeof(frame);
    sp = (sp & ~(uintptr_t)15u) - 8u;
    frame_addr = sp;
    if (on_alt && !sigframe_on_altstack(frame_addr)) {
        return -1;
    }

    fpu_save(area);
    fpu_to_linux(area, fpstate);
    if (copy_to_user((void __user *)fp_addr, fpstate, sizeof(fpstate)) != 0) {
        return -1;
    }

    k_memset(&frame, 0, sizeof(frame));
    frame.pretcode = (uintptr_t)sa->restorer;
    frame.uc.uc_flags = UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS;
    frame.uc.uc_stack.ss_sp = (uintptr_t)current_proc->altstack_sp;
    frame.uc.uc_stack.ss_size = (uintptr_t)current_proc->altstack_size;
    frame.uc.uc_stack.ss_flags =
        (int32_t)(!current_proc->altstack_on ? SS_DISABLE : (on_alt ? SS_ONSTACK : 0u));
    fill_sigcontext(&frame.uc.mc, tf, fp_addr, oldmask);
    frame.uc.sigmask = oldmask;
    sigframe_fill_siginfo(frame.info, signo, info);
    if (copy_to_user((void __user *)frame_addr, &frame, sizeof(frame)) != 0) {
        return -1;
    }

    /* The handler starts with the initial floating-point state, as a fresh
     * image does, and its arguments in RDI, RSI and RDX; RAX is 0 for a
     * handler declared without a prototype that takes a variable count. */
    fpu_reset();
    tf->rsp = frame_addr;
    tf->rip = (uintptr_t)sa->handler;
    tf->rdi = (uint64_t)signo;
    tf->rsi = frame_addr + (uintptr_t)((char *)frame.info - (char *)&frame);
    tf->rdx = frame_addr + (uintptr_t)((char *)&frame.uc - (char *)&frame);
    tf->rax = 0;
    tf->cs = GDT_USER_CODE;
    tf->ss = GDT_USER_DATA;
    tf->rflags &= ~(uint64_t)(EFLAGS_DF | EFLAGS_TF | EFLAGS_RF);
    return 0;
}

/**
 * Restore the registers rt_sigreturn finds in the frame below the stack
 * pointer: the handler's `ret` took `pretcode`, so the frame starts 8 bytes
 * under it. There is no plain sigreturn on x86-64, and a call without `rt`
 * is refused. A return address outside the lower half is refused too:
 * `iretq` itself would fault on it.
 */
int sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask)
{
    struct lx_rt_sigframe frame;
    uint8_t area[FPU_STATE_SIZE];
    uint8_t fpstate[FPU_LINUX_SIZE];
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
        if (copy_from_user(fpstate, (const void __user *)(uintptr_t)sc->fpstate, sizeof(fpstate)) !=
                0 ||
            fpu_from_linux(fpstate, area) != 0) {
            return -1;
        }
        fpu_restore(area);
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

    /* A frame's alternate stack comes back as sigaltstack would take it,
     * except that a stack in use or too small is left as it is. */
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
