/*
 * ptrace.c: the i686 part of ptrace(2) and of core files: the registers in
 * Linux i386's `user_regs_struct`, the words of its `struct user`, the
 * floating-point state in its three forms, the register sets of
 * PTRACE_GETREGSET, and the TLS entry.
 */

#include "../arch.h"
#include "../x86/debugreg.h"
#include "../x86/fpu.h"
#include "gdt.h"
#include "tls.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/coredump.h"
#include "../../proc/ptrace.h"

/* The requests this file answers. */
#define PTRACE_PEEKUSER        3
#define PTRACE_POKEUSER        6
#define PTRACE_GETREGS         12
#define PTRACE_SETREGS         13
#define PTRACE_GETFPREGS       14
#define PTRACE_SETFPREGS       15
#define PTRACE_GETFPXREGS      18
#define PTRACE_SETFPXREGS      19
#define PTRACE_GET_THREAD_AREA 25
#define PTRACE_SET_THREAD_AREA 26

/* The i386 `struct user`: the 17 words of `user_regs_struct`, then the
 * floating-point and accounting fields, and the eight debug registers at
 * byte 252, 284 bytes in all. */
enum {
    R_EBX,
    R_ECX,
    R_EDX,
    R_ESI,
    R_EDI,
    R_EBP,
    R_EAX,
    R_DS,
    R_ES,
    R_FS,
    R_GS,
    R_ORIG_EAX,
    R_EIP,
    R_CS,
    R_EFL,
    R_UESP,
    R_SS,
    R_COUNT
};
#define USER_FPVALID  68u
#define USER_DEBUGREG 252u
#define USER_SIZE     284u

/* The user selectors: code, data, and the TLS entry's. */
#define SEL_UCODE 0x1Bu
#define SEL_UDATA 0x23u
#define SEL_UTLS  0x33u

/* The flags a tracer may change, Linux's FLAG_MASK: CF, PF, AF, ZF, SF, TF,
 * DF, OF, NT, RF and AC. */
#define FLAG_MASK 0x00054DD5u
#define EFLAGS_TF 0x00000100u

/* The register set of the general registers, by its core note's type. */
#define NT_PRSTATUS 1u

void arch_regs_get(const process_t *t, unsigned long *r)
{
    const isr_params_t *tf = proc_user_frame(t);

    r[R_EBX] = tf->ebx;
    r[R_ECX] = tf->ecx;
    r[R_EDX] = tf->edx;
    r[R_ESI] = tf->esi;
    r[R_EDI] = tf->edi;
    r[R_EBP] = tf->ebp;
    r[R_EAX] = tf->eax;
    r[R_DS] = tf->ds & 0xFFFFu;
    r[R_ES] = tf->es & 0xFFFFu;
    r[R_FS] = tf->fs & 0xFFFFu;
    r[R_GS] = tf->gs & 0xFFFFu;
    r[R_ORIG_EAX] = (unsigned long)(long)t->orig_nr;
    r[R_EIP] = tf->eip;
    r[R_CS] = tf->cs & 0xFFFFu;
    r[R_EFL] = tf->eflags;
    r[R_UESP] = tf->user_esp;
    r[R_SS] = tf->user_ss & 0xFFFFu;
}

/**
 * Set the user register `i` of the stopped thread `t` to `v`, as Linux's
 * `putreg` does: the flags only where a program may change them, a data
 * segment register only to a selector this kernel has (null, user data, or
 * the TLS entry's), and CS and SS only to the ones they have, since a return
 * to user mode with any other would fault in the kernel. An orig_eax of -1
 * stops an interrupted system call from being restarted. 0, or -EIO.
 */
static int regs_set(process_t *t, int i, unsigned long v)
{
    isr_params_t *tf = proc_user_frame(t);

    switch (i) {
    case R_EBX:
        tf->ebx = v;
        break;
    case R_ECX:
        tf->ecx = v;
        break;
    case R_EDX:
        tf->edx = v;
        break;
    case R_ESI:
        tf->esi = v;
        break;
    case R_EDI:
        tf->edi = v;
        break;
    case R_EBP:
        tf->ebp = v;
        break;
    case R_EAX:
        tf->eax = v;
        break;
    case R_DS:
    case R_ES:
    case R_FS:
    case R_GS:
        v &= 0xFFFFu;
        if (v != 0 && v != SEL_UDATA && v != SEL_UTLS) {
            return -EIO;
        }
        if (i == R_DS) {
            tf->ds = v;
        } else if (i == R_ES) {
            tf->es = v;
        } else if (i == R_FS) {
            tf->fs = v;
        } else {
            tf->gs = v;
        }
        break;
    case R_ORIG_EAX:
        t->orig_nr = (int32_t)v;
        if ((int32_t)v == -1) {
            t->restart_nr = 0;
        }
        break;
    case R_EIP:
        tf->eip = v;
        break;
    case R_CS:
        if ((v & 0xFFFFu) != SEL_UCODE) {
            return -EIO;
        }
        break;
    case R_EFL:
        tf->eflags = (tf->eflags & ~FLAG_MASK) | (v & FLAG_MASK);
        break;
    case R_UESP:
        tf->user_esp = v;
        break;
    case R_SS:
        if ((v & 0xFFFFu) != SEL_UDATA) {
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
        tf->eflags |= EFLAGS_TF;
    } else {
        tf->eflags &= ~EFLAGS_TF;
    }
}

/** PTRACE_PEEKUSER: the word at byte `off` of `struct user`, the registers,
 * the valid mark of the floating-point state, or a debug register, into
 * `*out`; the other fields read as 0. -EIO for an offset outside the
 * structure or not aligned to a word. */
static int user_peek(const process_t *t, unsigned long off, unsigned long *out)
{
    unsigned long r[R_COUNT];

    if ((off & 3u) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    *out = 0;
    if (off < R_COUNT * 4u) {
        arch_regs_get(t, r);
        *out = r[off / 4u];
    } else if (off == USER_FPVALID) {
        *out = 1;
    } else if (off >= USER_DEBUGREG) {
        *out = debugreg_peek(t, (unsigned)((off - USER_DEBUGREG) / 4u));
    }
    return 0;
}

/** PTRACE_POKEUSER: set a register or a debug register; the other fields of
 * `struct user` are -EIO, and a debug register as `debugreg_poke` says. */
static int user_poke(process_t *t, unsigned long off, unsigned long v)
{
    if ((off & 3u) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    if (off < R_COUNT * 4u) {
        return regs_set(t, (int)(off / 4u), v);
    }
    if (off < USER_DEBUGREG) {
        return -EIO;
    }
    return debugreg_poke(t, (unsigned)((off - USER_DEBUGREG) / 4u), v);
}

/** PTRACE_GETFPREGS, SETFPREGS, GETFPXREGS and SETFPXREGS: copy the
 * floating-point state of `t`, the thread `pid`, to or from the user at
 * `data`, as the 108-byte `user_i387_struct` or, with `fx`, the 512-byte
 * `fxsave` image. -EIO for the image on a processor without FXSR, -EINVAL for
 * one `fxrstor` would fault on, -ESRCH for a thread that left its stop during
 * the copy. */
static int fp_access(uint32_t pid, process_t *t, uintptr_t data, int fx, int set)
{
    uint8_t buf[FX_SIZE];
    uint32_t len = fx ? FX_SIZE : FPU_ENV_SIZE;

    if (fx && !fpu_has_fxsr()) {
        return -EIO;
    }
    if (!set) {
        if (fx) {
            fpu_fx_get(t->fpu, buf);
        } else {
            fpu_env_get(t->fpu, buf);
        }
        return copy_to_user((void __user *)data, buf, len) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(buf, (const void __user *)data, len) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    if (fx) {
        return fpu_fx_set(t->fpu, buf) == 0 ? 0 : -EINVAL;
    }
    fpu_env_set(t->fpu, buf);
    return 0;
}

/** The register sets: NT_PRSTATUS, the `user_regs_struct`; NT_PRFPREG, the
 * `user_i387_struct`; NT_PRXFPREG, the `fxsave` image, which a processor
 * without FXSR lacks; and NT_X86_XSTATE, the XSAVE image, which one without
 * XSAVE lacks. */
long arch_regset_size(unsigned long type, uint32_t *unit)
{
    *unit = 4u;
    switch (type) {
    case NT_PRSTATUS:
        return (long)(R_COUNT * 4u);
    case NT_PRFPREG:
        return (long)FPU_ENV_SIZE;
    case NT_PRXFPREG:
        return fpu_has_fxsr() ? (long)FX_SIZE : -ENODEV;
    case NT_X86_XSTATE:
        *unit = 8u;
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
        fpu_env_get(t->fpu, out);
        break;
    case NT_PRXFPREG:
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
 * stopping at the first one `regs_set` refuses; the `user_i387_struct` in
 * part or whole, over what the thread has; the `fxsave` image whole,
 * -EINVAL for a part or for one `fxrstor` would fault on; and the XSAVE
 * image whole, -EFAULT for a part and -EINVAL for one `fpu_xstate_set`
 * refuses.
 */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    unsigned long r[R_COUNT];
    uint8_t env[FPU_ENV_SIZE];
    uint32_t i;
    int rc;

    switch (type) {
    case NT_PRSTATUS:
        k_memcpy(r, in, len);
        for (i = 0; i < len / 4u; i++) {
            rc = regs_set(t, (int)i, r[i]);
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    case NT_PRFPREG:
        fpu_env_get(t->fpu, env);
        k_memcpy(env, in, len);
        fpu_env_set(t->fpu, env);
        return 0;
    case NT_PRXFPREG:
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
 * PTRACE_GET_THREAD_AREA and PTRACE_SET_THREAD_AREA: the TLS entry `idx` of
 * `t`, the thread `pid`, as a `user_desc` at `data`.
 *
 * Linux i386 has three TLS entries, 6 to 8, and GDB reads and writes back all
 * three. The kernel has one, GDT_TLS_ENTRY; the other two read as Linux reads
 * an unused entry, and a write to them may only leave them unused, in either
 * of the two forms Linux takes for that.
 */
static int tls_access(uint32_t pid, process_t *t, unsigned long idx, uintptr_t data, int set)
{
    user_desc_t desc;
    uint32_t base;
    uint32_t kind;

    if (idx < GDT_TLS_ENTRY || idx > GDT_TLS_ENTRY + 2u) {
        return -EINVAL;
    }
    if (!set) {
        desc.entry_number = (uint32_t)idx;
        desc.base_addr = 0;
        desc.limit = 0;
        desc.flags = UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT;
        if (idx == GDT_TLS_ENTRY) {
            desc.base_addr = t->tls_base;
            desc.limit = 0xFFFFFu;
            desc.flags = UD_SEG_32BIT | UD_LIMIT_IN_PAGES | UD_USEABLE;
        }
        return copy_to_user((void __user *)data, &desc, sizeof(desc)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(&desc, (const void __user *)data, sizeof(desc)) != 0) {
        return -EFAULT;
    }
    if (idx != GDT_TLS_ENTRY) {
        kind = desc.flags & 0x7Fu;
        if (desc.base_addr != 0 || desc.limit != 0 ||
            (kind != 0 && kind != (UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT))) {
            return -EINVAL;
        }
        return ptrace_tracee(pid) ? 0 : -ESRCH;
    }
    desc.entry_number = (uint32_t)idx;
    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    t->tls_base = base;
    return 0;
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
        return fp_access(pid, t, data, 0, request == PTRACE_SETFPREGS);
    case PTRACE_GETFPXREGS:
    case PTRACE_SETFPXREGS:
        return fp_access(pid, t, data, 1, request == PTRACE_SETFPXREGS);
    case PTRACE_GET_THREAD_AREA:
    case PTRACE_SET_THREAD_AREA:
        return tls_access(pid, t, addr, data, request == PTRACE_SET_THREAD_AREA);
    default:
        return -EIO;
    }
}

uint32_t arch_core_fp_size(void)
{
    uint32_t xstate = fpu_xstate_size();

    return core_note_bytes(FPU_ENV_SIZE) + core_note_bytes(FX_SIZE) +
           (xstate != 0 ? core_note_bytes(xstate) : 0u);
}

/**
 * The core file's floating-point notes, as Linux writes them for an i386
 * thread: NT_PRFPREG, the `user_i387_struct`; NT_PRXFPREG, the `fxsave`
 * image, zero without FXSR; and with XSAVE NT_X86_XSTATE, the XSAVE image
 * with XCR0 in its software bytes (`fpu_xstate_view`), made from the area's
 * copy in the note itself. The note's description follows its header and
 * name, the bytes `core_note_bytes(0)` counts.
 */
void arch_core_fp_notes(uint8_t **p, const process_t *t)
{
    uint8_t buf[FX_SIZE];
    uint32_t xstate = fpu_xstate_size();
    uint8_t *desc;

    fpu_env_get(t->fpu, buf);
    core_note_put(p, "CORE", NT_PRFPREG, buf, FPU_ENV_SIZE);
    k_memset(buf, 0, sizeof(buf));
    if (fpu_has_fxsr()) {
        fpu_fx_get(t->fpu, buf);
    }
    core_note_put(p, "LINUX", NT_PRXFPREG, buf, FX_SIZE);
    if (xstate != 0) {
        desc = *p + core_note_bytes(0);
        core_note_put(p, "LINUX", NT_X86_XSTATE, t->fpu, xstate);
        fpu_xstate_view(desc);
    }
}
