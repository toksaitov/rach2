/*
 * ptrace.c: the x86-64 part of ptrace(2) and of core files: the registers
 * in Linux x86-64's `user_regs_struct`, the words of its `struct user`, the
 * floating-point state as `user_i387_struct`, which is the 512-byte
 * `fxsave64` image, the register sets of PTRACE_GETREGSET, the XSAVE image
 * among them, the FS and GS bases through PTRACE_ARCH_PRCTL, the TLS
 * entries through PTRACE_GET_THREAD_AREA, and a core file's floating-point
 * notes.
 *
 * An i386 tracer, one whose ptrace call is made by `int $0x80`, gets
 * Linux's i386 forms instead, as Linux's `ia32_arch_ptrace` gives them:
 * the 17 words of `user_regs_struct32`, the offsets of `struct user32`,
 * the 108-byte `user_i387_ia32_struct` and the `fxsave` image of
 * PTRACE_GETFPXREGS, whatever its tracee runs. The register sets follow the
 * tracee instead, as Linux's `task_user_regset_view` does: a thread that
 * runs 32-bit code, CS 0x23, has the i386 sets for any tracer.
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
#include "../../syscall.h"

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
#define PTRACE_ARCH_PRCTL      30

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
/* The register set of the general registers, by its core note's type, and
 * i386's set of the TLS entries. */
#define NT_PRSTATUS 1u
#define NT_386_TLS  0x200u

/* The i386 `user_regs_struct`, Linux's `user_regs_struct32`, 17 words of 4
 * bytes, which opens `struct user32`; that structure has the debug
 * registers at byte 252 and is 284 bytes long. */
enum {
    R32_EBX,
    R32_ECX,
    R32_EDX,
    R32_ESI,
    R32_EDI,
    R32_EBP,
    R32_EAX,
    R32_DS,
    R32_ES,
    R32_FS,
    R32_GS,
    R32_ORIG_EAX,
    R32_EIP,
    R32_CS,
    R32_EFLAGS,
    R32_ESP,
    R32_SS,
    R32_COUNT
};
#define USER32_DEBUGREG 252u
#define USER32_SIZE     284u
#define USER_DESC_SIZE  16u

/* The build stops unless the enum has the ARCH_GREGS words of cpu.h. */
typedef char user_regs_fit[(R_COUNT == ARCH_GREGS) ? 1 : -1];

/**
 * The running thread, whose registers a core dump of its own process reads,
 * has its bases and selectors read from the processor, since a selector it
 * loaded may have changed them since its record was written; a stopped
 * thread's were saved when it switched away.
 */
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
    r[R_FS_BASE] = t == current_proc ? msr_read(MSR_FS_BASE) : t->tls_base;
    r[R_GS_BASE] = t == current_proc ? msr_read(MSR_KERNEL_GS_BASE) : t->regs.gs_base;
    r[R_DS] = t == current_proc ? x86_64_ds_read() : t->regs.ds_sel;
    r[R_ES] = t == current_proc ? x86_64_es_read() : t->regs.es_sel;
    r[R_FS] = t == current_proc ? x86_64_fs_read() : t->regs.fs_sel;
    r[R_GS] = t == current_proc ? x86_64_gs_read() : t->regs.gs_sel;
}

/**
 * Set the DS, ES, FS or GS selector, by `i`, of the stopped thread `t` to
 * the low 16 bits of `value`, as Linux's `set_segment_reg` takes them, the
 * selector the switch in loads before the bases. The selector must be
 * null, at RPL 0 or 3, a user code or data selector, the selector at RPL 3
 * of one of the thread's TLS entries that holds a descriptor, or any of the
 * LDT at RPL 3, which the switch loads as null if it does not load. Linux
 * takes any selector at RPL 3; one of the GDT outside these would load a
 * kernel segment or the TSS. Returns 0, or -EIO.
 */
static int sel_set(process_t *t, int i, unsigned long value)
{
    unsigned long sel = value & 0xFFFFu, entry = sel >> 3;
    int tls = (sel & 7u) == 3u && entry >= GDT_TLS_MIN && entry <= GDT_TLS_MAX &&
              t->regs.tls[entry - GDT_TLS_MIN] != 0;

    if (sel != 0 && sel != 3 && sel != GDT_USER_DATA && sel != GDT_USER_CODE &&
        sel != GDT_USER_CODE32 && !tls && (sel & 7u) != 7u) {
        return -EIO;
    }
    switch (i) {
    case R_DS:
        t->regs.ds_sel = (uint16_t)sel;
        break;
    case R_ES:
        t->regs.es_sel = (uint16_t)sel;
        break;
    case R_FS:
        t->regs.fs_sel = (uint16_t)sel;
        break;
    default:
        t->regs.gs_sel = (uint16_t)sel;
    }
    return 0;
}

/**
 * Set the user register `i` of the stopped thread `t` to `v`, as Linux's
 * `putreg` does: the flags only where a program may change them, a base
 * only to a user address, CS to the user's 64-bit or 32-bit code selector
 * and SS to a nonzero selector at RPL 3, and DS, ES,
 * FS and GS only to a selector `sel_set` takes. An orig_rax of -1 stops an
 * interrupted system call from being restarted; it is kept in 32 bits, as
 * the system call number is. Returns 0, or -EIO.
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
        break;
    case R_RIP:
        tf->rip = v;
        break;
    case R_CS:
        if ((v & 0xFFFFu) != GDT_USER_CODE && (v & 0xFFFFu) != GDT_USER_CODE32 && (v & 7u) != 7u) {
            return -EIO;
        }
        tf->cs = v & 0xFFFFu;
        break;
    case R_EFLAGS:
        tf->rflags = (tf->rflags & ~(uint64_t)FLAG_MASK) | (v & FLAG_MASK);
        break;
    case R_RSP:
        tf->rsp = v;
        break;
    case R_SS:
        if ((v & 3u) != 3u) {
            return -EIO;
        }
        tf->ss = v & 0xFFFFu;
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
        return sel_set(t, i, v);
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

/* The native register of each word of `user_regs_struct32`. */
static const unsigned char r32_native[R32_COUNT] = {
    R_RBX, R_RCX, R_RDX,      R_RSI, R_RDI, R_RBP,    R_RAX, R_DS, R_ES,
    R_FS,  R_GS,  R_ORIG_RAX, R_RIP, R_CS,  R_EFLAGS, R_RSP, R_SS};

/** Fill `r` with the 17 words of `t`'s `user_regs_struct32`: the low halves
 * of its registers, as Linux's `getreg32` reads them. */
static void regs32_get(const process_t *t, uint32_t *r)
{
    unsigned long native[R_COUNT];
    int i;

    arch_regs_get(t, native);
    for (i = 0; i < R32_COUNT; i++) {
        r[i] = (uint32_t)native[r32_native[i]];
    }
}

/** Set word `i` of `t`'s `user_regs_struct32` to `v`, zero-extended into
 * its register as Linux's `putreg32` does, and through `regs_set`'s
 * checks; FS and GS also take the base their new selector gives, as an
 * i386 tracer on Linux expects, read from the 16 bits `sel_set` stored,
 * as Linux's `x86_fsgsbase_read_task` reads them. 0, or -EIO. */
static int regs32_set(process_t *t, int i, uint32_t v)
{
    int rc;

    if (i < 0 || i >= R32_COUNT) {
        return -EIO;
    }
    rc = regs_set(t, r32_native[i], v);

    if (rc == 0 && i == R32_FS) {
        t->tls_base = tls_selector_base(t, t->regs.fs_sel);
    } else if (rc == 0 && i == R32_GS) {
        t->regs.gs_base = tls_selector_base(t, t->regs.gs_sel);
    }
    return rc;
}

/** The DS selector of `t`, which the x87 environment of an i386 tracer's
 * view takes (`fpu_env32_get`). */
static uint32_t ds_of(const process_t *t)
{
    return t == current_proc ? x86_64_ds_read() : t->regs.ds_sel;
}

/** Whether `t` runs 32-bit code, the test of Linux's `user_64bit_mode`,
 * which chooses the i386 register sets. */
static int runs32(const process_t *t)
{
    return proc_user_frame(t)->cs != GDT_USER_CODE;
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
 * `user_i387_struct`, which is the `fxsave64` image. -EFAULT for a buffer
 * that cannot be copied, -EINVAL for an image `fxrstor64` would fault on,
 * and -ESRCH for a thread that left its stop during the copy. */
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

/** An i386 tracer's PTRACE_PEEKUSER: the 4-byte word at byte `off` of
 * `struct user32`, a register or a debug register, and 0 for its other
 * fields, as Linux's `getreg32`. -EIO for an offset not aligned to 4 or
 * outside the structure. */
static int user32_peek(const process_t *t, unsigned long off, uint32_t *out)
{
    uint32_t r[R32_COUNT];

    if ((off & 3u) != 0 || off >= USER32_SIZE) {
        return -EIO;
    }
    *out = 0;
    if (off < R32_COUNT * 4u) {
        regs32_get(t, r);
        *out = r[off / 4u];
    } else if (off >= USER32_DEBUGREG && off < USER32_DEBUGREG + 8u * 4u) {
        *out = (uint32_t)debugreg_peek(t, (unsigned)((off - USER32_DEBUGREG) / 4u));
    }
    return 0;
}

/** An i386 tracer's PTRACE_POKEUSER: set a register or a debug register;
 * the other fields of `struct user32` take the write and ignore it, as
 * Linux's `putreg32` does. -EIO as `user32_peek`, or from the register. */
static int user32_poke(process_t *t, unsigned long off, uint32_t v)
{
    if ((off & 3u) != 0 || off >= USER32_SIZE) {
        return -EIO;
    }
    if (off < R32_COUNT * 4u) {
        return regs32_set(t, (int)(off / 4u), v);
    }
    if (off >= USER32_DEBUGREG && off < USER32_DEBUGREG + 8u * 4u) {
        return debugreg_poke(t, (unsigned)((off - USER32_DEBUGREG) / 4u), v);
    }
    return 0;
}

/** Set the words of `user_regs_struct32` from the `len` bytes at `in`, in
 * order, stopping at the first one `regs32_set` refuses, as Linux's
 * `genregs32_set` does. */
static long regs32_set_all(process_t *t, const uint8_t *in, uint32_t len)
{
    uint32_t v;
    uint32_t i;
    int rc;

    for (i = 0; i < len / 4u; i++) {
        k_memcpy(&v, in + i * 4u, sizeof(v));
        rc = regs32_set(t, (int)i, v);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** An i386 tracer's PTRACE_GETFPREGS and PTRACE_SETFPREGS: the 108-byte
 * `user_i387_ia32_struct` of `t`, the thread `pid`, at `data`, with the
 * selectors `fpu_env32_get` takes. -EFAULT, or -ESRCH for a thread that
 * left its stop during the copy. */
static int fp32_access(uint32_t pid, process_t *t, uintptr_t data, int set)
{
    uint8_t env[FPU_ENV_SIZE];

    if (!set) {
        fpu_env32_get(t->fpu, env, (uint32_t)proc_user_frame(t)->cs, ds_of(t));
        return copy_to_user((void __user *)data, env, sizeof(env)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(env, (const void __user *)data, sizeof(env)) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    fpu_env32_set(t->fpu, env);
    return 0;
}

/**
 * PTRACE_GET_THREAD_AREA and PTRACE_SET_THREAD_AREA: TLS entry `idx`, 12 to
 * 14, of `t`, the thread `pid`, as the `user_desc` at `data`, as Linux's
 * `do_get_thread_area` and `do_set_thread_area` take it from a tracer: a
 * write takes the descriptor set_thread_area takes, and FS or GS holding
 * the entry's selector, at any RPL, take its base. -EIO for an entry
 * negative as an int, -EINVAL for another entry or a descriptor refused,
 * -EFAULT, -ESRCH.
 */
static int tls_access(uint32_t pid, process_t *t, unsigned long idx, uintptr_t data, int set)
{
    struct user_desc u;
    int rc;

    if ((int)idx < 0) {
        return -EIO;
    }
    if (!set) {
        rc = tls_entry_get(t, (uint32_t)idx, &u);
        if (rc != 0) {
            return rc;
        }
        return copy_to_user((void __user *)data, &u, sizeof(u)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(&u, (const void __user *)data, sizeof(u)) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    rc = tls_entry_set(t, (uint32_t)idx, &u);
    if (rc == 0 && TLS_SEL_NAMES(t->regs.fs_sel, idx)) {
        t->tls_base = u.base_addr;
    }
    if (rc == 0 && TLS_SEL_NAMES(t->regs.gs_sel, idx)) {
        t->regs.gs_base = u.base_addr;
    }
    return rc;
}

/** The i386 register sets of a thread that runs 32-bit code, Linux's
 * `user_x86_32_view`: NT_PRSTATUS, `user_regs_struct32`; NT_PRFPREG,
 * `user_i387_ia32_struct`; NT_PRXFPREG, the `fxsave` image; NT_X86_XSTATE;
 * and NT_386_TLS, the three TLS entries as `user_desc`s. */
static long regset32_size(unsigned long type, uint32_t *unit)
{
    *unit = 4u;
    switch (type) {
    case NT_PRSTATUS:
        return (long)(R32_COUNT * 4u);
    case NT_PRFPREG:
        return (long)FPU_ENV_SIZE;
    case NT_PRXFPREG:
        return (long)FX_SIZE;
    case NT_X86_XSTATE:
        *unit = 8u;
        return fpu_xstate_size() != 0 ? (long)fpu_xstate_size() : -ENODEV;
    case NT_386_TLS:
        *unit = USER_DESC_SIZE;
        return (long)(ARCH_TLS_ENTRIES * USER_DESC_SIZE);
    default:
        return -EINVAL;
    }
}

/** Write the i386 set `type` of `t` into `out`. */
static void regset32_get(const process_t *t, unsigned long type, uint8_t *out)
{
    uint32_t r[R32_COUNT];
    struct user_desc u;
    int i;

    switch (type) {
    case NT_PRSTATUS:
        regs32_get(t, r);
        k_memcpy(out, r, sizeof(r));
        break;
    case NT_PRFPREG:
        fpu_env32_get(t->fpu, out, (uint32_t)proc_user_frame(t)->cs, ds_of(t));
        break;
    case NT_PRXFPREG:
        fpu_fx_get(t->fpu, out);
        break;
    case NT_386_TLS:
        for (i = 0; i < ARCH_TLS_ENTRIES; i++) {
            tls_entry_get(t, (uint32_t)(GDT_TLS_MIN + i), &u);
            k_memcpy(out + i * USER_DESC_SIZE, &u, sizeof(u));
        }
        break;
    default:
        k_memcpy(out, t->fpu, fpu_xstate_size());
        fpu_xstate_view(out);
        break;
    }
}

/**
 * Set the i386 set `type` of `t` from `len` bytes of `in`, as Linux's sets
 * of `user_x86_32_view` take them: the registers in order, stopping at the
 * first refused; the `user_i387_ia32_struct` and the `fxsave` image only
 * whole, -EINVAL otherwise or for an image `fxrstor` would fault on; the
 * TLS entries from the first, each a descriptor set_thread_area would
 * take, or none is set and -EINVAL; and the XSAVE image as the native set.
 */
static long regset32_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    struct user_desc u[ARCH_TLS_ENTRIES];
    uint32_t i;

    switch (type) {
    case NT_PRSTATUS:
        if (len > R32_COUNT * 4u || len % 4u != 0) {
            return -EINVAL;
        }
        return regs32_set_all(t, in, len);
    case NT_PRFPREG:
        if (len != FPU_ENV_SIZE) {
            return -EINVAL;
        }
        fpu_env32_set(t->fpu, in);
        return 0;
    case NT_PRXFPREG:
        if (len != FX_SIZE) {
            return -EINVAL;
        }
        return fpu_fx_set(t->fpu, in) == 0 ? 0 : -EINVAL;
    case NT_386_TLS:
        if (len > sizeof(u) || len % USER_DESC_SIZE != 0) {
            return -EINVAL;
        }
        k_memcpy(u, in, len);
        for (i = 0; i < len / USER_DESC_SIZE; i++) {
            if (!tls_desc_valid(&u[i])) {
                return -EINVAL;
            }
        }
        for (i = 0; i < len / USER_DESC_SIZE; i++) {
            tls_entry_set(t, (uint32_t)(GDT_TLS_MIN + i), &u[i]);
            if (TLS_SEL_NAMES(t->regs.fs_sel, GDT_TLS_MIN + i)) {
                t->tls_base = tls_selector_base(t, t->regs.fs_sel);
            }
            if (TLS_SEL_NAMES(t->regs.gs_sel, GDT_TLS_MIN + i)) {
                t->regs.gs_base = tls_selector_base(t, t->regs.gs_sel);
            }
        }
        return 0;
    default:
        if (len != fpu_xstate_size()) {
            return -EFAULT;
        }
        return fpu_xstate_set(t->fpu, in) == 0 ? 0 : -EINVAL;
    }
}

/** The register sets of a thread that runs 64-bit code: NT_PRSTATUS, the
 * `user_regs_struct`; NT_PRFPREG, the `fxsave64` image; and NT_X86_XSTATE,
 * the XSAVE image, which a processor without XSAVE lacks. A thread that
 * runs 32-bit code has the i386 sets (`regset32_size`). */
long arch_regset_size(const process_t *t, unsigned long type, uint32_t *unit)
{
    if (runs32(t)) {
        return regset32_size(type, unit);
    }
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

    if (runs32(t)) {
        regset32_get(t, type, out);
        return;
    }
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
 * refuses. A thread that runs 32-bit code takes the i386 sets
 * (`regset32_set`).
 */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    unsigned long r[R_COUNT];
    uint32_t i;
    int rc;

    if (runs32(t)) {
        return regset32_set(t, type, in, len);
    }
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
 * arch_prctl's own order. Setting a base sets that register's selector to
 * null, as arch_prctl does. -EPERM for a base at or above USER_ADDR_MAX,
 * -EFAULT for an address that cannot be written, -EINVAL for another code.
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
            t->regs.fs_sel = 0;
            t->tls_base = addr;
        } else {
            t->regs.gs_sel = 0;
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

/**
 * An i386 tracer's requests, as Linux's `ia32_arch_ptrace` answers them:
 * PEEKUSER, POKEUSER, GETREGS and SETREGS in `struct user32`'s 4-byte
 * words, GETFPREGS and SETFPREGS as `user_i387_ia32_struct`, GETFPXREGS
 * and SETFPXREGS as the `fxsave` image, and the TLS entries; any other,
 * PTRACE_ARCH_PRCTL among them, is -EIO.
 */
static long ia32_arch_ptrace(process_t *t, uint32_t pid, long request, unsigned long addr,
                             unsigned long data)
{
    uint32_t word;
    uint32_t r[R32_COUNT];
    uint8_t buf[R32_COUNT * 4u];
    int rc;

    switch (request) {
    case PTRACE_PEEKUSER:
        rc = user32_peek(t, addr, &word);
        if (rc == 0 && copy_to_user((void __user *)data, &word, sizeof(word)) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKEUSER:
        return user32_poke(t, addr, (uint32_t)data);
    case PTRACE_GETREGS:
        regs32_get(t, r);
        return copy_to_user((void __user *)data, r, sizeof(r)) == 0 ? 0 : -EFAULT;
    case PTRACE_SETREGS:
        if (copy_from_user(buf, (const void __user *)data, sizeof(buf)) != 0) {
            return -EFAULT;
        }
        t = ptrace_tracee(pid);
        return t ? regs32_set_all(t, buf, sizeof(buf)) : -ESRCH;
    case PTRACE_GETFPREGS:
    case PTRACE_SETFPREGS:
        return fp32_access(pid, t, data, request == PTRACE_SETFPREGS);
    case PTRACE_GETFPXREGS:
    case PTRACE_SETFPXREGS:
        return fp_access(pid, t, data, request == PTRACE_SETFPXREGS);
    case PTRACE_GET_THREAD_AREA:
    case PTRACE_SET_THREAD_AREA:
        return tls_access(pid, t, addr, data, request == PTRACE_SET_THREAD_AREA);
    default:
        return -EIO;
    }
}

/**
 * x86-64's requests are PEEKUSER, POKEUSER, GETREGS, SETREGS, GETFPREGS,
 * SETFPREGS, ARCH_PRCTL and, as on a Linux with i386 emulation,
 * GET_THREAD_AREA and SET_THREAD_AREA; an i386 tracer's are
 * `ia32_arch_ptrace`'s. A request that copies from the tracer looks the
 * tracee up again by `pid` after the copy, which may sleep, and answers
 * -ESRCH when it has left its stop. SETREGS sets the registers in order and
 * stops at the first one `regs_set` refuses, with those before it set.
 */
long arch_ptrace(process_t *t, uint32_t pid, long request, unsigned long addr, unsigned long data)
{
    unsigned long word;
    unsigned long r[R_COUNT];
    int rc;
    int i;

    if (in_compat_syscall()) {
        return ia32_arch_ptrace(t, pid, request, addr, data);
    }
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
