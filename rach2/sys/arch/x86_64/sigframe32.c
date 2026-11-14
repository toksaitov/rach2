/*
 * sigframe32.c: the signal frames of i386 handlers on the x86-64 kernel,
 * Linux's `sigframe_ia32` and `rt_sigframe_ia32` with their 32-bit
 * floating-point state, the returns from them, and the i386 siginfo.
 *
 * A handler gets these frames when its action was installed by an i386
 * call, which marks it SA_IA32_ABI (`signal_action`), as Linux decides by
 * its `is_ia32_frame`: with SA_SIGINFO the rt frame, else the plain one,
 * whose sigcontext holds signals 1 to 32 of the mask and `extramask` the
 * rest. The handler runs in 32-bit code, CS 0x23, with SS, DS and ES the
 * user's data selector and FS and GS as they were, and returns through
 * sigreturn or rt_sigreturn made by `int $0x80`, which come here
 * (`sigframe_restore`).
 */

#include "ia32.h"
#include "percpu.h"

#include "../x86/fpu.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../proc/sigframe.h"

#if RACH2_IA32

/* The i386 `struct sigcontext`, Linux's `sigcontext_32`, 88 bytes, with
 * each 16-bit selector in the low half of a 32-bit slot. */
struct sigcontext32 {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss;
    uint32_t fpstate; /* the user address of the _fpstate_32, or 0 */
    uint32_t oldmask; /* signals 1 to 32 of the mask to restore */
    uint32_t cr2;
};

/* Linux's `ucontext_ia32`, 116 bytes: the `compat_stack_t` inline and a
 * 64-bit mask in two words. */
struct ucontext32 {
    uint32_t uc_flags;
    uint32_t uc_link;
    uint32_t ss_sp;
    int32_t ss_flags;
    uint32_t ss_size;
    struct sigcontext32 mc;
    uint32_t sigmask[2];
};

/* Linux's `rt_sigframe_ia32`, 268 bytes: the return address, the handler's
 * three arguments, the siginfo, the ucontext and the return code. */
struct rt_sigframe32 {
    uint32_t pretcode;
    int32_t sig;
    uint32_t pinfo;
    uint32_t puc;
    uint8_t info[SIGINFO_SIZE];
    struct ucontext32 uc;
    uint8_t retcode[8];
};

/* Linux's `sigframe_ia32`, 732 bytes. The unused floating-point area keeps
 * `extramask` where old programs found it; the state itself lies above the
 * frame. */
struct sigframe32 {
    uint32_t pretcode;
    int32_t sig;
    struct sigcontext32 sc;
    uint8_t fpstate_unused[624];
    uint32_t extramask; /* signals 33 to 64 of the mask to restore */
    uint8_t retcode[8];
};

/* The build stops unless the frames have Linux's sizes. */
typedef char
    sigframe32_sizes_ok[(sizeof(struct sigcontext32) == 88 && sizeof(struct ucontext32) == 116 &&
                         sizeof(struct rt_sigframe32) == 268 && sizeof(struct sigframe32) == 732)
                            ? 1
                            : -1];

#define SS_ONSTACK 1u
#define SS_DISABLE 2u

/* The flags a frame may change, the trap flags, and the i386 alignment:
 * the handler's stack pointer plus four, where its return address ends, is
 * a multiple of 16. */
#define FIX_EFLAGS  0x00050DD5u
#define EFLAGS_TF   0x00000100u
#define EFLAGS_DF   0x00000400u
#define EFLAGS_RF   0x00010000u
#define FRAME_ALIGN 16u

/* The end of what a 32-bit pointer reaches. */
#define LOW_4G 0x100000000UL

/* The access rights `lar` reads (Intel SDM Vol. 3A, 3.4.5): present, the S
 * bit of a code or data segment, DPL 3, and of the type the code bit and,
 * for code, the readable bit. */
#define AR_P        0x8000u
#define AR_S        0x1000u
#define AR_DPL3     0x6000u
#define AR_CODE     0x0800u
#define AR_READABLE 0x0200u

/* `movl $173, %eax; int $0x80` and `popl %eax; movl $119, %eax; int $0x80`,
 * the returns of a frame without a restorer, which debuggers also look for
 * to know a signal frame. */
static const uint8_t rt_retcode[8] = {0xB8, 0xAD, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x00};
static const uint8_t plain_retcode[8] = {0x58, 0xB8, 0x77, 0x00, 0x00, 0x00, 0xCD, 0x80};

/** Store `v` at `p`, which need not be aligned. */
static void put32(uint8_t *p, uint32_t v)
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

/* The layouts of a siginfo's union, Linux 6.12's enum siginfo_layout, of
 * those the kernel keeps fields for; the fault layouts of a machine check, a
 * bound and a perf event, which nothing here makes, are SIL_FAULT's. */
#define SIL_KILL  0
#define SIL_TIMER 1
#define SIL_POLL  2
#define SIL_FAULT 3
#define SIL_CHLD  4
#define SIL_RT    5
#define SIL_SYS   6

/* The last code of each signal whose codes have a layout of their own
 * (include/uapi/asm-generic/siginfo.h); SIGPOLL's last is any signal's last
 * of SIL_POLL. */
#define NSIGILL  11
#define NSIGFPE  15
#define NSIGSEGV 10
#define NSIGBUS  5
#define NSIGTRAP 6
#define NSIGCHLD 6
#define NSIGPOLL 6
#define NSIGSYS  2

/* The code of a SIGIO a descriptor's owner is sent, Linux's SI_SIGIO. */
#define SI_SIGIO (-5)

/**
 * The layout of the union of a siginfo with the signal `signo` and the code
 * `code`, as Linux 6.12's siginfo_layout (kernel/signal.c) chooses it: a
 * code between SI_USER and SI_KERNEL has the layout of its signal's codes up
 * to the last of them, and otherwise SIL_POLL up to NSIGPOLL, whatever the
 * signal, and SIL_KILL past it; SI_TIMER is SIL_TIMER, SI_SIGIO SIL_POLL and
 * any other negative code SIL_RT; SI_USER and SI_KERNEL and the codes above
 * it are SIL_KILL.
 */
static int siginfo_layout(int signo, int32_t code)
{
    int32_t last = 0;
    int layout = SIL_FAULT;

    if (code > SI_USER && code < SI_KERNEL) {
        switch (signo) {
        case SIGILL:
            last = NSIGILL;
            break;
        case SIGFPE:
            last = NSIGFPE;
            break;
        case SIGSEGV:
            last = NSIGSEGV;
            break;
        case SIGBUS:
            last = NSIGBUS;
            break;
        case SIGTRAP:
            last = NSIGTRAP;
            break;
        case SIGCHLD:
            last = NSIGCHLD;
            layout = SIL_CHLD;
            break;
        case SIGSYS:
            last = NSIGSYS;
            layout = SIL_SYS;
            break;
        default:
            break;
        }
        if (code <= last) {
            return layout;
        }
        return code <= NSIGPOLL ? SIL_POLL : SIL_KILL;
    }
    if (code == SI_TIMER) {
        return SIL_TIMER;
    }
    if (code == SI_SIGIO) {
        return SIL_POLL;
    }
    return code < 0 ? SIL_RT : SIL_KILL;
}

/**
 * The i386 siginfo: signo, errno and code, then at byte 12 the union, with
 * the fields of the layout the signal and code have (`siginfo_layout`), as
 * Linux's copy_siginfo_to_external32 writes them, and no others: the
 * sender's or the thread's pid and uid at 12 and 16 (SIL_KILL); those and,
 * at 20, a child's status (SIL_CHLD) or a queued signal's si_value
 * (SIL_RT), or a timer's id, overrun and si_value (SIL_TIMER); a band's low
 * 32 bits, kept in pid, and a descriptor, kept in status, at 12 and 16
 * (SIL_POLL); a fault's address, and for SIGSEGV with SEGV_PKUERR the key at
 * 20, `si_pkey` (SIL_FAULT); and a seccomp SIGSYS's call address, call
 * number at 16 and architecture at 20 (SIL_SYS). Addresses and values keep
 * their low 32 bits. A siginfo marked `notify` that has a fault's layout,
 * the SIGTRAP of a plain system call stop, gives its pid as the address:
 * Linux's union holds the pid and uid there, and the i386 address is the
 * low half of that word.
 */
void ia32_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    k_memset(out, 0, SIGINFO_SIZE);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    switch (siginfo_layout(signo, info->code)) {
    case SIL_FAULT:
        put32(out + 12, info->notify ? info->pid : (uint32_t)info->addr);
        if (signo == SIGSEGV && info->code == SEGV_PKUERR) {
            put32(out + 20, info->pkey);
        }
        break;
    case SIL_SYS:
        put32(out + 12, (uint32_t)info->addr);
        put32(out + 16, info->uid);
        put32(out + 20, (uint32_t)info->status);
        break;
    case SIL_POLL:
        put32(out + 12, info->pid);
        put32(out + 16, (uint32_t)info->status);
        break;
    case SIL_TIMER:
    case SIL_RT:
        put32(out + 12, info->pid);
        put32(out + 16, info->uid);
        put32(out + 20, (uint32_t)info->value);
        break;
    case SIL_CHLD:
        put32(out + 12, info->pid);
        put32(out + 16, info->uid);
        put32(out + 20, (uint32_t)info->status);
        break;
    default:
        put32(out + 12, info->pid);
        put32(out + 16, info->uid);
        break;
    }
}

/** The fields of the i386 siginfo at `in` that `ia32_fill_siginfo` writes
 * for the layout its signal and code have, and no others, as Linux's
 * post_copy_siginfo_from_user32 reads them: a pointer or value is
 * zero-extended, as Linux's `compat_ptr` extends it, and a band
 * sign-extended into pid and uid, where the native layout's 64-bit band
 * is. A queued signal's or a timer's si_value is kept in status as well,
 * where the native layout of a code that is not SI_TIMER, SI_QUEUE or
 * SI_MESGQ reads it. */
void ia32_take_siginfo(const uint8_t *in, struct k_siginfo *info)
{
    int signo = (int)get32(in);
    int32_t code = (int32_t)get32(in + 8);

    k_memset(info, 0, sizeof(*info));
    info->err = (int32_t)get32(in + 4);
    info->code = code;
    switch (siginfo_layout(signo, code)) {
    case SIL_FAULT:
        info->addr = get32(in + 12);
        if (signo == SIGSEGV && code == SEGV_PKUERR) {
            info->pkey = get32(in + 20);
        }
        break;
    case SIL_SYS:
        info->addr = get32(in + 12);
        info->pid = (uint32_t)info->addr;
        info->uid = get32(in + 16);
        info->status = (int32_t)get32(in + 20);
        break;
    case SIL_POLL:
        info->pid = get32(in + 12);
        info->uid = (info->pid & 0x80000000u) ? 0xFFFFFFFFu : 0;
        info->status = (int32_t)get32(in + 16);
        break;
    case SIL_TIMER:
    case SIL_RT:
        info->pid = get32(in + 12);
        info->uid = get32(in + 16);
        info->value = get32(in + 20);
        info->status = (int32_t)get32(in + 20);
        break;
    case SIL_CHLD:
        info->pid = get32(in + 12);
        info->uid = get32(in + 16);
        info->status = (int32_t)get32(in + 20);
        break;
    default:
        info->pid = get32(in + 12);
        info->uid = get32(in + 16);
        break;
    }
}

/** Whether the selector `sel` at RPL 3 may be loaded into DS, ES, FS or GS
 * from the kernel: null, or a present data or readable code segment at DPL
 * 3, which `lar` reads as 0 where the descriptor is out of reach. */
static int seg_loadable(unsigned int sel)
{
    unsigned int ar;

    if ((sel & ~3u) == 0) {
        return 1;
    }
    if (sel & 4u) {
        ldt_switch(current_proc->mm);
    }
    ar = x86_64_lar(sel);
    if ((ar & (AR_P | AR_S | AR_DPL3)) != (AR_P | AR_S | AR_DPL3)) {
        return 0;
    }
    return !(ar & AR_CODE) || (ar & AR_READABLE);
}

/** The selector a frame's `sel` loads at RPL 3, or the null selector where
 * that would fault, as the fault fixup of Linux's `loadsegment` loads. */
static unsigned int seg_from_frame(uint32_t sel)
{
    unsigned int s = (sel & 0xFFFFu) | 3u;

    return seg_loadable(s) ? s : 0u;
}

/**
 * Load the frame's GS, FS, DS and ES into each register that holds another
 * selector, as Linux's `reload_segments` does on return from an i386
 * frame. A load into FS or GS sets that base, to 0 for the null selector,
 * and the record takes it, as the switch away would.
 */
static void segs_restore(const struct sigcontext32 *sc)
{
    unsigned int gs = seg_from_frame(sc->gs), fs = seg_from_frame(sc->fs);
    unsigned int ds = seg_from_frame(sc->ds), es = seg_from_frame(sc->es);

    if (x86_64_gs_read() != gs) {
        x86_64_gs_load(gs);
        if ((gs & ~3u) == 0) {
            msr_write(MSR_KERNEL_GS_BASE, 0);
        }
        current_proc->regs.gs_base = msr_read(MSR_KERNEL_GS_BASE);
    }
    if (x86_64_fs_read() != fs) {
        x86_64_fs_load(fs);
        if ((fs & ~3u) == 0) {
            msr_write(MSR_FS_BASE, 0);
        }
        current_proc->tls_base = msr_read(MSR_FS_BASE);
    }
    if (x86_64_ds_read() != ds) {
        x86_64_ds_load(ds);
    }
    if (x86_64_es_read() != es) {
        x86_64_es_load(es);
    }
}

/**
 * Fill `sc` from the frame `tf` and the selectors the processor holds, with
 * the number, error code and address of the current process's last
 * processor exception, the user address of the floating-point state
 * `fp_addr` and signals 1 to 32 of `oldmask`, as Linux's
 * `__unsafe_setup_sigcontext32` does.
 */
static void fill_sigcontext32(struct sigcontext32 *sc, const isr_params_t *tf, uint32_t fp_addr,
                              uint64_t oldmask)
{
    sc->gs = x86_64_gs_read() & 0xFFFFu;
    sc->fs = x86_64_fs_read() & 0xFFFFu;
    sc->es = x86_64_es_read() & 0xFFFFu;
    sc->ds = x86_64_ds_read() & 0xFFFFu;
    sc->edi = (uint32_t)tf->rdi;
    sc->esi = (uint32_t)tf->rsi;
    sc->ebp = (uint32_t)tf->rbp;
    sc->esp = (uint32_t)tf->rsp;
    sc->ebx = (uint32_t)tf->rbx;
    sc->edx = (uint32_t)tf->rdx;
    sc->ecx = (uint32_t)tf->rcx;
    sc->eax = (uint32_t)tf->rax;
    sc->trapno = (uint32_t)current_proc->trap_nr;
    sc->err = (uint32_t)current_proc->trap_err;
    sc->eip = (uint32_t)tf->rip;
    sc->cs = (uint32_t)(tf->cs & 0xFFFFu);
    sc->eflags = (uint32_t)tf->rflags;
    sc->esp_at_signal = (uint32_t)tf->rsp;
    sc->ss = (uint32_t)(tf->ss & 0xFFFFu);
    sc->fpstate = fp_addr;
    sc->oldmask = (uint32_t)oldmask;
    sc->cr2 = (uint32_t)current_proc->trap_addr;
}

/** The larger frame's alignment costs up to 15 bytes. */
uintptr_t ia32_sigframe_bytes(void)
{
    return fpu_frame32_bytes() + sizeof(struct sigframe32) + FRAME_ALIGN - 1u;
}

/** The address a frame without SA_RESTORER returns to: the vDSO's
 * `__kernel_sigreturn` or `__kernel_rt_sigreturn`, or the frame's own
 * `retcode` at `retcode` when the process has no 32-bit vDSO. */
static uint32_t restorer_of(const struct k_sigaction *sa, int rt, uintptr_t retcode)
{
    uintptr_t vdso;

    if (sa->flags & SA_RESTORER) {
        return (uint32_t)sa->restorer;
    }
    vdso = ia32_vdso_sigreturn(rt);
    return (uint32_t)(vdso != 0 ? vdso : retcode);
}

/**
 * The frame goes below the interrupted stack pointer, an i386 stack having
 * no red zone, or at the top of the alternate stack under SA_ONSTACK, with
 * the floating-point state above it (`fpu_frame32_place`). Linux's switch
 * to the stack in `sa_restorer` for a non-flat SS applies without
 * SA_ONSTACK or SA_RESTORER, when not already on the alternate stack.
 * A frame any byte of which
 * lies at or past 4 GiB, which a 64-bit program's stack can put it, is
 * refused, where Linux writes it and its handler faults on the truncated
 * stack pointer. The restorer is SA_RESTORER's, else the 32-bit vDSO's,
 * else the frame's `retcode`, as Linux takes it.
 *
 * The frame is written with every protection key open and keeps the
 * interrupted code's PKRU, as `sigframe_setup` writes the native one.
 * The handler starts with the initial floating-point state, the signal in
 * EAX and, for the rt frame, the siginfo's and ucontext's addresses in EDX
 * and ECX, as `-mregparm=3` reads them besides the stack; DF, TF and RF are
 * cleared.
 */
int ia32_sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                        const struct k_siginfo *info, uint64_t oldmask)
{
    union {
        struct rt_sigframe32 rt;
        struct sigframe32 plain;
    } frame;
    int rt = (sa->flags & SA_SIGINFO) != 0;
    int on_alt, was_on;
    uintptr_t sp = tf->rsp;
    uintptr_t fp_addr, frame_addr, size;
    uint32_t pkru;

    on_alt = was_on = sigframe_on_altstack(tf->rsp);
    if ((sa->flags & SA_ONSTACK) && current_proc->altstack_on && !on_alt) {
        sp = (uintptr_t)current_proc->altstack_sp + (uintptr_t)current_proc->altstack_size;
        on_alt = 1;
    } else if (!(sa->flags & (SA_ONSTACK | SA_RESTORER)) && !was_on && tf->ss != GDT_USER_DATA &&
               sa->restorer) {
        sp = (uintptr_t)sa->restorer;
        on_alt = 1;
    }
    fp_addr = fpu_frame32_place(sp);
    size = rt ? sizeof(frame.rt) : sizeof(frame.plain);
    sp = fp_addr - size;
    sp = ((sp + 4u) & ~(uintptr_t)(FRAME_ALIGN - 1u)) - 4u;
    frame_addr = sp;
    if (on_alt && !sigframe_on_altstack(frame_addr)) {
        return -1;
    }
    if (frame_addr >= LOW_4G || fp_addr >= LOW_4G || fpu_frame32_bytes() > LOW_4G - fp_addr) {
        return -1;
    }

    fpu_save(current_proc->fpu);
    pkru = fpu_pkru();
    fpu_pkru_set(0);
    if (fpu_frame32_put(fp_addr, current_proc->fpu, (uint32_t)tf->cs, x86_64_ds_read()) != 0) {
        fpu_pkru_set(pkru);
        return -1;
    }

    k_memset(&frame, 0, sizeof(frame));
    if (rt) {
        frame.rt.sig = signo;
        frame.rt.pinfo = (uint32_t)(frame_addr + offsetof(struct rt_sigframe32, info));
        frame.rt.puc = (uint32_t)(frame_addr + offsetof(struct rt_sigframe32, uc));
        ia32_fill_siginfo(frame.rt.info, signo, info);
        frame.rt.uc.uc_flags = fpu_frame_uc_flags();
        frame.rt.uc.ss_sp = (uint32_t)current_proc->altstack_sp;
        frame.rt.uc.ss_size = (uint32_t)current_proc->altstack_size;
        frame.rt.uc.ss_flags =
            (int32_t)(!current_proc->altstack_on ? SS_DISABLE : (was_on ? SS_ONSTACK : 0u));
        fill_sigcontext32(&frame.rt.uc.mc, tf, (uint32_t)fp_addr, oldmask);
        frame.rt.uc.sigmask[0] = (uint32_t)oldmask;
        frame.rt.uc.sigmask[1] = (uint32_t)(oldmask >> 32);
        k_memcpy(frame.rt.retcode, rt_retcode, sizeof(rt_retcode));
        frame.rt.pretcode =
            restorer_of(sa, 1, frame_addr + offsetof(struct rt_sigframe32, retcode));
    } else {
        frame.plain.sig = signo;
        fill_sigcontext32(&frame.plain.sc, tf, (uint32_t)fp_addr, oldmask);
        frame.plain.extramask = (uint32_t)(oldmask >> 32);
        k_memcpy(frame.plain.retcode, plain_retcode, sizeof(plain_retcode));
        frame.plain.pretcode =
            restorer_of(sa, 0, frame_addr + offsetof(struct sigframe32, retcode));
    }
    if (copy_to_user((void __user *)frame_addr, &frame, size) != 0) {
        fpu_pkru_set(pkru);
        return -1;
    }

    fpu_reset();
    tf->rsp = frame_addr;
    tf->rip = (uint32_t)sa->handler;
    tf->rax = (uint64_t)signo;
    tf->rdx = rt ? frame.rt.pinfo : 0u;
    tf->rcx = rt ? frame.rt.puc : 0u;
    if (x86_64_ds_read() != GDT_USER_DATA) {
        x86_64_ds_load(GDT_USER_DATA);
    }
    if (x86_64_es_read() != GDT_USER_DATA) {
        x86_64_es_load(GDT_USER_DATA);
    }
    tf->cs = GDT_USER_CODE32;
    tf->ss = GDT_USER_DATA;
    tf->rflags &= ~(uint64_t)(EFLAGS_DF | EFLAGS_TF | EFLAGS_RF);
    return 0;
}

/**
 * The rt frame starts 4 bytes below the stack pointer, since the handler's
 * `ret` took `pretcode`, and the plain one 8, since its restorer popped the
 * signal number too. The i386 registers come back zero-extended and R8 to
 * R15 stay as they are, as Linux's `ia32_restore_sigcontext` leaves them;
 * CS and SS are the frame's at RPL 3, and a CS that is not the user's
 * 32-bit or 64-bit code selector or a code segment of the LDT
 * (`ldt_code_valid`), or an SS `iretq` cannot load, is refused with -1, the
 * SIGSEGV of Linux's faulting `iretq`. GS, FS, DS and ES are
 * loaded as `segs_restore` says. A frame without floating-point state gets
 * the initial one. The rt frame's alternate stack comes back as
 * `sigframe_restore` takes it.
 */
int ia32_sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask)
{
    union {
        struct rt_sigframe32 rt;
        struct sigframe32 plain;
    } frame;
    const struct sigcontext32 *sc;
    uintptr_t frame_addr;
    uint64_t cs, ss;

    if (rt) {
        frame_addr = tf->rsp - 4u;
        if (copy_from_user(&frame.rt, (const void __user *)frame_addr, sizeof(frame.rt)) != 0) {
            return -1;
        }
        sc = &frame.rt.uc.mc;
        *mask = (uint64_t)frame.rt.uc.sigmask[0] | ((uint64_t)frame.rt.uc.sigmask[1] << 32);
    } else {
        frame_addr = tf->rsp - 8u;
        if (copy_from_user(&frame.plain, (const void __user *)frame_addr, sizeof(frame.plain)) !=
            0) {
            return -1;
        }
        sc = &frame.plain.sc;
        *mask = (uint64_t)sc->oldmask | ((uint64_t)frame.plain.extramask << 32);
    }
    cs = (uint64_t)((sc->cs & 0xFFFFu) | 3u);
    ss = (uint64_t)((sc->ss & 0xFFFFu) | 3u);
    if ((cs != GDT_USER_CODE32 && cs != GDT_USER_CODE && !ldt_code_valid((unsigned int)cs)) ||
        !sigframe_ss_valid((unsigned int)ss)) {
        return -1;
    }

    if (sc->fpstate != 0) {
        if (fpu_frame32_load(sc->fpstate) != 0) {
            return -1;
        }
    } else {
        fpu_reset();
    }

    tf->rbx = sc->ebx;
    tf->rcx = sc->ecx;
    tf->rdx = sc->edx;
    tf->rsi = sc->esi;
    tf->rdi = sc->edi;
    tf->rbp = sc->ebp;
    tf->rax = sc->eax;
    tf->rsp = sc->esp;
    tf->rip = sc->eip;
    tf->cs = cs;
    tf->ss = ss;
    tf->rflags = (tf->rflags & ~(uint64_t)FIX_EFLAGS) | (sc->eflags & FIX_EFLAGS);
    segs_restore(sc);

    if (rt && !sigframe_on_altstack(tf->rsp)) {
        if ((uint32_t)frame.rt.uc.ss_flags & SS_DISABLE) {
            current_proc->altstack_on = 0;
        } else if (frame.rt.uc.ss_size >= 2048u) {
            current_proc->altstack_sp = frame.rt.uc.ss_sp;
            current_proc->altstack_size = frame.rt.uc.ss_size;
            current_proc->altstack_on = 1;
        }
    }
    return 0;
}

#endif /* RACH2_IA32 */
