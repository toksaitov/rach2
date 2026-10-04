/*
 * sigframe.c: the Linux i386 signal frames, as sigframe.h describes them.
 */

#include "../../proc/sigframe.h"

#include "../x86/fpu.h"
#include "../../proc/process.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "gdt.h"

/* The i386 `struct sigcontext`, with each 16-bit selector in the low half of
 * a 32-bit slot, as the kernel writes it. */
struct lx_sigcontext {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss;
    uint32_t fpstate; /* the user address of the _fpstate_32 */
    uint32_t oldmask; /* signals 1 to 32 of the mask to restore */
    uint32_t cr2;
};

struct lx_ucontext {
    uint32_t uc_flags;
    uint32_t uc_link;
    uint32_t ss_sp; /* uc_stack */
    uint32_t ss_flags;
    uint32_t ss_size;
    struct lx_sigcontext mc;
    uint32_t sigmask[2];
};

struct lx_rt_sigframe {
    uint32_t pretcode;
    int32_t sig;
    uint32_t pinfo;
    uint32_t puc;
    uint8_t info[128];
    struct lx_ucontext uc;
    uint8_t retcode[8];
};

/* The plain frame keeps an unused floating-point area where old programs
 * found one, so `extramask` stays at the offset they read it from. */
struct lx_sigframe {
    uint32_t pretcode;
    int32_t sig;
    struct lx_sigcontext sc;
    uint8_t fpstate_unused[FPU_LINUX_SIZE];
    uint32_t extramask; /* signals 33 to 64 of the mask to restore */
    uint8_t retcode[8];
};

#define SS_ONSTACK 1u
#define SS_DISABLE 2u

/* The EFLAGS bits a frame may change: the arithmetic flags, the direction
 * and trap flags, resume and alignment check. */
#define FIX_EFLAGS 0x00050DD5u
#define EFLAGS_TF  0x00000100u
#define EFLAGS_DF  0x00000400u
#define EFLAGS_RF  0x00010000u

/* The i386 frame alignment: the handler's stack pointer plus four, where its
 * return address ends, is a multiple of 16. */
#define FRAME_ALIGN 16u

/* `movl $173, %eax; int $0x80`, and `popl %eax; movl $119, %eax; int $0x80`:
 * the returns a frame without SA_RESTORER uses, and what debuggers look for
 * to know a signal frame. */
static const uint8_t rt_retcode[8] = {0xB8, 0xAD, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x00};
static const uint8_t plain_retcode[8] = {0x58, 0xB8, 0x77, 0x00, 0x00, 0x00, 0xCD, 0x80};

int sigframe_on_altstack(uintptr_t sp)
{
    return current_proc->altstack_on &&
           sp - current_proc->altstack_sp < current_proc->altstack_size;
}

/* One of the two data selectors this kernel has: flat user data or the TLS
 * entry. The return path loads the selector at ring 0, so one past the GDT's
 * limit would fault the kernel, not the process. */
static uint32_t user_seg(uint32_t sel)
{
    uint32_t s = sel & 0xFFFFu;

    return (s == GDT_USER_DATA_SELECTOR || s == GDT_TLS_SELECTOR) ? s : GDT_USER_DATA_SELECTOR;
}

static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    int fault = (signo == SIGSEGV || signo == SIGBUS || signo == SIGILL || signo == SIGFPE ||
                 signo == SIGTRAP) &&
                info->code > 0;

    k_memset(out, 0, 128);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    if (fault) {
        put32(out + 12, info->addr);
        return;
    }
    put32(out + 12, info->pid);
    put32(out + 16, info->uid);
    put32(out + 20, (uint32_t)info->status);
}

void sigframe_take_siginfo(const uint8_t *in, struct k_siginfo *info)
{
    int32_t code;

    k_memset(info, 0, sizeof(*info));
    k_memcpy(&code, in + 8, 4);
    info->code = code;
    k_memcpy(&info->pid, in + 12, 4);
    k_memcpy(&info->uid, in + 16, 4);
    k_memcpy(&info->status, in + 20, 4);
    k_memcpy(&info->addr, in + 12, 4);
    info->value = (uint32_t)info->status;
}

static void fill_sigcontext(struct lx_sigcontext *sc, const isr_params_t *tf, uint32_t fp_addr,
                            uint64_t oldmask)
{
    sc->gs = tf->gs & 0xFFFFu;
    sc->fs = tf->fs & 0xFFFFu;
    sc->es = tf->es & 0xFFFFu;
    sc->ds = tf->ds & 0xFFFFu;
    sc->edi = tf->edi;
    sc->esi = tf->esi;
    sc->ebp = tf->ebp;
    sc->esp = tf->user_esp;
    sc->ebx = tf->ebx;
    sc->edx = tf->edx;
    sc->ecx = tf->ecx;
    sc->eax = tf->eax;
    sc->trapno = current_proc->trap_nr;
    sc->err = current_proc->trap_err;
    sc->eip = tf->eip;
    sc->cs = tf->cs & 0xFFFFu;
    sc->eflags = tf->eflags;
    sc->esp_at_signal = tf->user_esp;
    sc->ss = tf->user_ss & 0xFFFFu;
    sc->fpstate = fp_addr;
    sc->oldmask = (uint32_t)oldmask;
    sc->cr2 = current_proc->trap_addr;
}

uintptr_t sigframe_stack_bytes(void)
{
    uintptr_t rt = sizeof(struct lx_rt_sigframe), plain = sizeof(struct lx_sigframe);

    return fpu_frame_bytes() + (rt > plain ? rt : plain) + FRAME_ALIGN - 1u;
}

int sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                   const struct k_siginfo *info, uint64_t oldmask)
{
    union {
        struct lx_rt_sigframe rt;
        struct lx_sigframe plain;
    } frame;
    int rt = (sa->flags & SA_SIGINFO) != 0;
    int on_alt = sigframe_on_altstack(tf->user_esp);
    uint32_t sp = tf->user_esp;
    uint32_t fp_addr, frame_addr, size, restorer;

    if ((sa->flags & SA_ONSTACK) && current_proc->altstack_on && !on_alt) {
        sp = current_proc->altstack_sp + current_proc->altstack_size;
        on_alt = 1;
    }
    fp_addr = (uint32_t)fpu_frame_place(sp);
    sp = fp_addr;
    size = rt ? sizeof(frame.rt) : sizeof(frame.plain);
    sp -= size;
    sp = ((sp + 4u) & ~(FRAME_ALIGN - 1u)) - 4u;
    frame_addr = sp;
    if (on_alt && !sigframe_on_altstack(frame_addr)) {
        return -1;
    }

    fpu_save(current_proc->fpu);
    if (fpu_frame_put(fp_addr, current_proc->fpu) != 0) {
        return -1;
    }

    k_memset(&frame, 0, sizeof(frame));
    if (rt) {
        restorer = frame_addr + (uint32_t)((char *)frame.rt.retcode - (char *)&frame.rt);
        frame.rt.sig = signo;
        frame.rt.pinfo = frame_addr + (uint32_t)((char *)frame.rt.info - (char *)&frame.rt);
        frame.rt.puc = frame_addr + (uint32_t)((char *)&frame.rt.uc - (char *)&frame.rt);
        sigframe_fill_siginfo(frame.rt.info, signo, info);
        frame.rt.uc.uc_flags = fpu_frame_uc_flags();
        frame.rt.uc.ss_sp = current_proc->altstack_sp;
        frame.rt.uc.ss_size = current_proc->altstack_size;
        frame.rt.uc.ss_flags = !current_proc->altstack_on ? SS_DISABLE : (on_alt ? SS_ONSTACK : 0u);
        fill_sigcontext(&frame.rt.uc.mc, tf, fp_addr, oldmask);
        frame.rt.uc.sigmask[0] = (uint32_t)oldmask;
        frame.rt.uc.sigmask[1] = (uint32_t)(oldmask >> 32);
        k_memcpy(frame.rt.retcode, rt_retcode, sizeof(rt_retcode));
        frame.rt.pretcode = (sa->flags & SA_RESTORER) ? sa->restorer : restorer;
    } else {
        restorer = frame_addr + (uint32_t)((char *)frame.plain.retcode - (char *)&frame.plain);
        frame.plain.sig = signo;
        fill_sigcontext(&frame.plain.sc, tf, fp_addr, oldmask);
        frame.plain.extramask = (uint32_t)(oldmask >> 32);
        k_memcpy(frame.plain.retcode, plain_retcode, sizeof(plain_retcode));
        frame.plain.pretcode = (sa->flags & SA_RESTORER) ? sa->restorer : restorer;
    }
    if (copy_to_user((void __user *)frame_addr, &frame, size) != 0) {
        return -1;
    }

    /* The handler starts with the initial floating-point state, as a fresh
     * image does. */
    fpu_reset();
    tf->user_esp = frame_addr;
    tf->eip = sa->handler;
    tf->eax = (uint32_t)signo;
    tf->edx = rt ? frame.rt.pinfo : 0;
    tf->ecx = rt ? frame.rt.puc : 0;
    tf->cs = GDT_USER_CODE_SELECTOR;
    tf->user_ss = GDT_USER_DATA_SELECTOR;
    tf->ds = GDT_USER_DATA_SELECTOR;
    tf->es = GDT_USER_DATA_SELECTOR;
    tf->eflags &= ~(EFLAGS_DF | EFLAGS_TF | EFLAGS_RF);
    return 0;
}

int sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask)
{
    union {
        struct lx_rt_sigframe rt;
        struct lx_sigframe plain;
    } frame;
    const struct lx_sigcontext *sc;
    uint32_t frame_addr;

    if (rt) {
        frame_addr = tf->user_esp - 4u;
        if (copy_from_user(&frame.rt, (const void __user *)frame_addr, sizeof(frame.rt)) != 0) {
            return -1;
        }
        sc = &frame.rt.uc.mc;
        *mask = (uint64_t)frame.rt.uc.sigmask[0] | ((uint64_t)frame.rt.uc.sigmask[1] << 32);
    } else {
        frame_addr = tf->user_esp - 8u;
        if (copy_from_user(&frame.plain, (const void __user *)frame_addr, sizeof(frame.plain)) !=
            0) {
            return -1;
        }
        sc = &frame.plain.sc;
        *mask = (uint64_t)sc->oldmask | ((uint64_t)frame.plain.extramask << 32);
    }

    if (sc->fpstate != 0) {
        if (fpu_frame_load(sc->fpstate) != 0) {
            return -1;
        }
    } else {
        fpu_reset();
    }

    tf->edi = sc->edi;
    tf->esi = sc->esi;
    tf->ebp = sc->ebp;
    tf->ebx = sc->ebx;
    tf->edx = sc->edx;
    tf->ecx = sc->ecx;
    tf->eax = sc->eax;
    tf->eip = sc->eip;
    tf->user_esp = sc->esp;
    tf->cs = GDT_USER_CODE_SELECTOR;
    tf->user_ss = GDT_USER_DATA_SELECTOR;
    tf->ds = user_seg(sc->ds);
    tf->es = user_seg(sc->es);
    tf->fs = user_seg(sc->fs);
    tf->gs = user_seg(sc->gs);
    tf->eflags = (tf->eflags & ~FIX_EFLAGS) | (sc->eflags & FIX_EFLAGS);

    /* A frame's alternate stack comes back as sigaltstack would take it,
     * except that a stack in use or too small is left as it is. */
    if (rt && !sigframe_on_altstack(tf->user_esp)) {
        if (frame.rt.uc.ss_flags & SS_DISABLE) {
            current_proc->altstack_on = 0;
        } else if (frame.rt.uc.ss_size >= 2048u) {
            current_proc->altstack_sp = frame.rt.uc.ss_sp;
            current_proc->altstack_size = frame.rt.uc.ss_size;
            current_proc->altstack_on = 1;
        }
    }
    return 0;
}
