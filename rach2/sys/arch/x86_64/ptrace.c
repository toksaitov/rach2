/*
 * ptrace.c: the x86-64 part of ptrace(2) and of core files: the registers
 * in Linux x86-64's `user_regs_struct`, the words of its `struct user`, the
 * floating-point state as `user_i387_struct`, the 512-byte `fxsave64`
 * image, and as the XSAVE image, the register sets of PTRACE_GETREGSET, and
 * the FS and GS bases through PTRACE_ARCH_PRCTL.
 */

#include "../arch.h"
#include "../x86/debugreg.h"
#include "../x86/fpu.h"
#include "percpu.h"
#include "tls.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/coredump.h"
#include "../../proc/ptrace.h"

/* The requests this file answers. */
#define PTRACE_PEEKUSER   3
#define PTRACE_POKEUSER   6
#define PTRACE_GETREGS    12
#define PTRACE_SETREGS    13
#define PTRACE_GETFPREGS  14
#define PTRACE_SETFPREGS  15
#define PTRACE_ARCH_PRCTL 30

/* The x86-64 `struct user`: the 27 words of `user_regs_struct`, then the
 * floating-point and accounting fields, the eight debug registers at byte
 * 848, and two words more, 928 bytes in all. */
enum {
    R_R15,
    R_R14,
    R_R13,
    R_R12,
    R_RBP,
    R_RBX,
    R_R11,
    R_R10,
    R_R9,
    R_R8,
    R_RAX,
    R_RCX,
    R_RDX,
    R_RSI,
    R_RDI,
    R_ORIG_RAX,
    R_RIP,
    R_CS,
    R_EFLAGS,
    R_RSP,
    R_SS,
    R_FS_BASE,
    R_GS_BASE,
    R_DS,
    R_ES,
    R_FS,
    R_GS,
    R_COUNT
};
#define USER_FPVALID  216u
#define USER_DEBUGREG 848u
#define USER_SIZE     928u

/* The flags a tracer may change, Linux's FLAG_MASK: CF, PF, AF, ZF, SF, TF,
 * DF, OF, NT, RF and AC. */
#define FLAG_MASK   0x00054DD5u
#define RFLAGS_TF   0x00000100u
#define WORD        8u
/* The register set of the general registers, by its core note's type. */
#define NT_PRSTATUS 1u

typedef char user_regs_fit[(R_COUNT == ARCH_GREGS) ? 1 : -1];

void arch_regs_get(const process_t *t, unsigned long *r)
{
    const isr_params_t *tf = proc_user_frame(t);

    r[R_R15] = tf->r15;
    r[R_R14] = tf->r14;
    r[R_R13] = tf->r13;
    r[R_R12] = tf->r12;
    r[R_RBP] = tf->rbp;
    r[R_RBX] = tf->rbx;
    r[R_R11] = tf->r11;
    r[R_R10] = tf->r10;
    r[R_R9] = tf->r9;
    r[R_R8] = tf->r8;
    r[R_RAX] = tf->rax;
    r[R_RCX] = tf->rcx;
    r[R_RDX] = tf->rdx;
    r[R_RSI] = tf->rsi;
    r[R_RDI] = tf->rdi;
    r[R_ORIG_RAX] = (unsigned long)(long)t->orig_nr;
    r[R_RIP] = tf->rip;
    r[R_CS] = tf->cs & 0xFFFFu;
    r[R_EFLAGS] = tf->rflags;
    r[R_RSP] = tf->rsp;
    r[R_SS] = tf->ss & 0xFFFFu;
    /* The running thread's bases are its MSRs, which a selector it loaded
     * may have changed since its record was written; a stopped thread's
     * were saved when it switched away. */
    r[R_FS_BASE] = t == current_proc ? msr_read(MSR_FS_BASE) : t->tls_base;
    r[R_GS_BASE] = t == current_proc ? msr_read(MSR_KERNEL_GS_BASE) : t->regs.gs_base;
    r[R_DS] = 0;
    r[R_ES] = 0;
    r[R_FS] = 0;
    r[R_GS] = 0;
}

/**
 * Set the user register `i` of the stopped thread `t` to `v`, as Linux's
 * `putreg` does: the flags only where a program may change them, a base
 * only to a user address, and CS and SS only to the ones they have, since a
 * return to user mode with any other would fault in the kernel. The data
 * segment registers are null in a 64-bit program and stay so; a tracer may
 * write back the null it read, or the user data selector. An orig_rax of
 * -1 stops an interrupted system call from being restarted. 0, or -EIO.
 */
static int regs_set(process_t *t, int i, unsigned long v)
{
    isr_params_t *tf = proc_user_frame(t);

    switch (i) {
    case R_R15:
        tf->r15 = v;
        break;
    case R_R14:
        tf->r14 = v;
        break;
    case R_R13:
        tf->r13 = v;
        break;
    case R_R12:
        tf->r12 = v;
        break;
    case R_RBP:
        tf->rbp = v;
        break;
    case R_RBX:
        tf->rbx = v;
        break;
    case R_R11:
        tf->r11 = v;
        break;
    case R_R10:
        tf->r10 = v;
        break;
    case R_R9:
        tf->r9 = v;
        break;
    case R_R8:
        tf->r8 = v;
        break;
    case R_RAX:
        tf->rax = v;
        break;
    case R_RCX:
        tf->rcx = v;
        break;
    case R_RDX:
        tf->rdx = v;
        break;
    case R_RSI:
        tf->rsi = v;
        break;
    case R_RDI:
        tf->rdi = v;
        break;
    case R_ORIG_RAX:
        t->orig_nr = (int32_t)v;
        if ((int32_t)v == -1) {
            t->restart_nr = 0;
        }
        break;
    case R_RIP:
        tf->rip = v;
        break;
    case R_CS:
        if ((v & 0xFFFFu) != GDT_USER_CODE) {
            return -EIO;
        }
        break;
    case R_EFLAGS:
        tf->rflags = (tf->rflags & ~(uint64_t)FLAG_MASK) | (v & FLAG_MASK);
        break;
    case R_RSP:
        tf->rsp = v;
        break;
    case R_SS:
        if ((v & 0xFFFFu) != GDT_USER_DATA) {
            return -EIO;
        }
        break;
    case R_FS_BASE:
    case R_GS_BASE:
        if (v >= USER_ADDR_MAX) {
            return -EIO;
        }
        if (i == R_FS_BASE) {
            t->tls_base = v;
        } else {
            t->regs.gs_base = v;
        }
        break;
    case R_DS:
    case R_ES:
    case R_FS:
    case R_GS:
        v &= 0xFFFFu;
        if (v != 0 && v != GDT_USER_DATA) {
            return -EIO;
        }
        break;
    default:
        return -EIO;
    }
    return 0;
}

void arch_ptrace_step(process_t *t, int on)
{
    isr_params_t *tf = proc_user_frame(t);

    if (on) {
        tf->rflags |= RFLAGS_TF;
    } else {
        tf->rflags &= ~(uint64_t)RFLAGS_TF;
    }
}

/** PTRACE_PEEKUSER: the word at byte `off` of `struct user`, the registers,
 * the valid mark of the floating-point state, or a debug register, into
 * `*out`; the other fields read as 0. -EIO for an offset outside the
 * structure or not aligned to a word. */
static int user_peek(const process_t *t, unsigned long off, unsigned long *out)
{
    unsigned long r[R_COUNT];

    if ((off & (WORD - 1u)) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    *out = 0;
    if (off < R_COUNT * WORD) {
        arch_regs_get(t, r);
        *out = r[off / WORD];
    } else if (off == USER_FPVALID) {
        *out = 1;
    } else if (off >= USER_DEBUGREG && off < USER_DEBUGREG + 8u * WORD) {
        *out = debugreg_peek(t, (unsigned)((off - USER_DEBUGREG) / WORD));
    }
    return 0;
}

/** PTRACE_POKEUSER: set a register or a debug register; the other fields of
 * `struct user` are -EIO, and a debug register as `debugreg_poke` says. */
static int user_poke(process_t *t, unsigned long off, unsigned long v)
{
    if ((off & (WORD - 1u)) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    if (off < R_COUNT * WORD) {
        return regs_set(t, (int)(off / WORD), v);
    }
    if (off < USER_DEBUGREG || off >= USER_DEBUGREG + 8u * WORD) {
        return -EIO;
    }
    return debugreg_poke(t, (unsigned)((off - USER_DEBUGREG) / WORD), v);
}

/** PTRACE_GETFPREGS and PTRACE_SETFPREGS: copy the floating-point state of
 * `t`, the thread `pid`, to or from the user at `data` as the 512-byte
 * `user_i387_struct`, which is the `fxsave64` image. -EINVAL for one
 * `fxrstor64` would fault on, -ESRCH for a thread that left its stop
 * during the copy. */
static int fp_access(uint32_t pid, process_t *t, uintptr_t data, int set)
{
    uint8_t buf[FX_SIZE];

    if (!set) {
        fpu_fx_get(t->fpu, buf);
        return copy_to_user((void __user *)data, buf, sizeof(buf)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(buf, (const void __user *)data, sizeof(buf)) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    return fpu_fx_set(t->fpu, buf) == 0 ? 0 : -EINVAL;
}

/** The register sets: NT_PRSTATUS, the `user_regs_struct`; NT_PRFPREG, the
 * `fxsave64` image; and NT_X86_XSTATE, the XSAVE image, which a processor
 * without XSAVE lacks. */
long arch_regset_size(unsigned long type, uint32_t *unit)
{
    *unit = WORD;
    switch (type) {
    case NT_PRSTATUS:
        return (long)(R_COUNT * WORD);
    case NT_PRFPREG:
        return (long)FX_SIZE;
    case NT_X86_XSTATE:
        return fpu_xstate_size() != 0 ? (long)fpu_xstate_size() : -ENODEV;
    default:
        return -EINVAL;
    }
}

void arch_regset_get(const process_t *t, unsigned long type, uint8_t *out)
{
    unsigned long r[R_COUNT];

    switch (type) {
    case NT_PRSTATUS:
        arch_regs_get(t, r);
        k_memcpy(out, r, sizeof(r));
        break;
    case NT_PRFPREG:
        fpu_fx_get(t->fpu, out);
        break;
    default:
        k_memcpy(out, t->fpu, fpu_xstate_size());
        fpu_xstate_view(out);
        break;
    }
}

/**
 * As Linux sets them: the registers one at a time, as many as `len` holds,
 * stopping at the first one `regs_set` refuses; the `fxsave64` image whole,
 * -EINVAL for a part or for one `fxrstor64` would fault on; and the XSAVE
 * image whole, -EFAULT for a part and -EINVAL for one `fpu_xstate_set`
 * refuses.
 */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    unsigned long r[R_COUNT];
    uint32_t i;
    int rc;

    switch (type) {
    case NT_PRSTATUS:
        k_memcpy(r, in, len);
        for (i = 0; i < len / WORD; i++) {
            rc = regs_set(t, (int)i, r[i]);
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    case NT_PRFPREG:
        if (len != FX_SIZE) {
            return -EINVAL;
        }
        return fpu_fx_set(t->fpu, in) == 0 ? 0 : -EINVAL;
    default:
        if (len != fpu_xstate_size()) {
            return -EFAULT;
        }
        return fpu_xstate_set(t->fpu, in) == 0 ? 0 : -EINVAL;
    }
}

/**
 * PTRACE_ARCH_PRCTL: arch_prctl(2) on the stopped thread `t`, with the base,
 * or the user address to store it at, in `addr` and the operation in
 * `code`, which ptrace(2) takes in its `addr` and `data`, the reverse of
 * arch_prctl's own order.
 */
static int prctl_access(process_t *t, unsigned long addr, unsigned long code)
{
    uint64_t base;

    switch (code) {
    case ARCH_SET_FS:
    case ARCH_SET_GS:
        if (addr >= USER_ADDR_MAX) {
            return -EPERM;
        }
        if (code == ARCH_SET_FS) {
            t->tls_base = addr;
        } else {
            t->regs.gs_base = addr;
        }
        return 0;
    case ARCH_GET_FS:
    case ARCH_GET_GS:
        base = code == ARCH_GET_FS ? t->tls_base : t->regs.gs_base;
        return copy_to_user((void __user *)addr, &base, sizeof(base)) == 0 ? 0 : -EFAULT;
    default:
        return -EINVAL;
    }
}

long arch_ptrace(process_t *t, uint32_t pid, long request, unsigned long addr, unsigned long data)
{
    unsigned long word;
    unsigned long r[R_COUNT];
    int rc;
    int i;

    switch (request) {
    case PTRACE_PEEKUSER:
        rc = user_peek(t, addr, &word);
        if (rc == 0 && copy_to_user((void __user *)data, &word, sizeof(word)) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKEUSER:
        return user_poke(t, addr, data);
    case PTRACE_GETREGS:
        arch_regs_get(t, r);
        return copy_to_user((void __user *)data, r, sizeof(r)) == 0 ? 0 : -EFAULT;
    case PTRACE_SETREGS:
        if (copy_from_user(r, (const void __user *)data, sizeof(r)) != 0) {
            return -EFAULT;
        }
        t = ptrace_tracee(pid);
        if (!t) {
            return -ESRCH;
        }
        for (i = 0; i < R_COUNT; i++) {
            rc = regs_set(t, i, r[i]);
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    case PTRACE_GETFPREGS:
    case PTRACE_SETFPREGS:
        return fp_access(pid, t, data, request == PTRACE_SETFPREGS);
    case PTRACE_ARCH_PRCTL:
        return prctl_access(t, addr, data);
    default:
        return -EIO;
    }
}

uint32_t arch_core_fp_size(void)
{
    uint32_t xstate = fpu_xstate_size();

    return core_note_bytes(FX_SIZE) + (xstate != 0 ? core_note_bytes(xstate) : 0u);
}

/**
 * The core file's floating-point notes, as Linux writes them for an x86-64
 * thread: NT_PRFPREG, the `fxsave64` image, and with XSAVE NT_X86_XSTATE,
 * the XSAVE image with XCR0 in its software bytes (`fpu_xstate_view`), made
 * from the area's copy in the note itself. The note's description follows
 * its header and name, the bytes `core_note_bytes(0)` counts.
 */
void arch_core_fp_notes(uint8_t **p, const process_t *t)
{
    uint8_t fx[FX_SIZE];
    uint32_t xstate = fpu_xstate_size();
    uint8_t *desc;

    fpu_fx_get(t->fpu, fx);
    core_note_put(p, "CORE", NT_PRFPREG, fx, FX_SIZE);
    if (xstate != 0) {
        desc = *p + core_note_bytes(0);
        core_note_put(p, "LINUX", NT_X86_XSTATE, t->fpu, xstate);
        fpu_xstate_view(desc);
    }
}
