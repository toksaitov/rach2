/*
 * seccomp.c: the system call filters of seccomp(2).
 *
 * A filter is a classic BPF program, the instruction set of the BSD packet
 * filter, which seccomp runs over `struct seccomp_data` in place of a
 * packet: the call's number, the architecture, the instruction pointer and
 * the six arguments, each widened to 64 bits. The program may load those
 * words, compute in its accumulator A and index X with sixteen scratch
 * words, and jump only forward, so it always ends, at a return of the
 * action. The check at the install refuses anything else, as Linux's
 * `seccomp_check_filter` does.
 */

#include "seccomp.h"
#include "ptrace.h"
#include "tgroup.h"
#include "../../kernel.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/* The actions, the high 16 bits of a filter's return; the low 16 are data. */
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_USER_NOTIF   0x7fc00000u
#define SECCOMP_RET_TRACE        0x7ff00000u
#define SECCOMP_RET_LOG          0x7ffc0000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define SECCOMP_RET_ACTION_FULL  0xffff0000u
#define SECCOMP_RET_DATA         0x0000ffffu

/* seccomp(2)'s operations and the flags of SECCOMP_SET_MODE_FILTER. */
#define SECCOMP_SET_MODE_STRICT  0u
#define SECCOMP_SET_MODE_FILTER  1u
#define SECCOMP_GET_ACTION_AVAIL 2u
#define SECCOMP_GET_NOTIF_SIZES  3u

#define SECCOMP_FILTER_FLAG_TSYNC       0x01u
#define SECCOMP_FILTER_FLAG_LOG         0x02u
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW  0x04u
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH 0x10u
#define SECCOMP_FILTER_FLAGS                                                                \
    (SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | SECCOMP_FILTER_FLAG_SPEC_ALLOW | \
     SECCOMP_FILTER_FLAG_TSYNC_ESRCH)

/* SIGSYS's code for a call a filter trapped. The architecture the filters
 * see is AUDIT_ARCH, from the architecture's cpu.h. */
#define SYS_SECCOMP 1

/* Linux's limits: instructions in one program, and in a chain, each filter
 * counted with four more. */
#define BPF_MAXINSNS       4096u
#define MAX_INSNS_PER_PATH 32768u
#define BPF_MEMWORDS       16u
#define MAX_ERRNO          4095u

/* The size of `struct seccomp_data`: nr, arch, the instruction pointer and
 * six arguments. */
#define SECCOMP_DATA_SIZE 64u

/* The classic BPF encoding. */
#define BPF_CLASS(c) ((c) & 0x07u)
#define BPF_LD       0x00u
#define BPF_LDX      0x01u
#define BPF_ST       0x02u
#define BPF_STX      0x03u
#define BPF_ALU      0x04u
#define BPF_JMP      0x05u
#define BPF_RET      0x06u
#define BPF_MISC     0x07u
#define BPF_W        0x00u
#define BPF_IMM      0x00u
#define BPF_ABS      0x20u
#define BPF_MEM      0x60u
#define BPF_LEN      0x80u
#define BPF_K        0x00u
#define BPF_X        0x08u
#define BPF_A        0x10u
#define BPF_OP(c)    ((c) & 0xf0u)
#define BPF_ADD      0x00u
#define BPF_SUB      0x10u
#define BPF_MUL      0x20u
#define BPF_DIV      0x30u
#define BPF_OR       0x40u
#define BPF_AND      0x50u
#define BPF_LSH      0x60u
#define BPF_RSH      0x70u
#define BPF_NEG      0x80u
#define BPF_MOD      0x90u
#define BPF_XOR      0xa0u
#define BPF_JA       0x00u
#define BPF_JEQ      0x10u
#define BPF_JGT      0x20u
#define BPF_JGE      0x30u
#define BPF_JSET     0x40u
#define BPF_TAX      0x00u
#define BPF_TXA      0x80u

/* The calls the strict mode allows: read, write, exit and sigreturn. */
#define NR_EXIT      1u
#define NR_READ      3u
#define NR_WRITE     4u
#define NR_SIGRETURN 119u

/* One instruction, as `struct sock_filter` has it. */
struct sock_filter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};

struct seccomp_filter {
    uint32_t refs;
    struct seccomp_filter *prev;
    uint32_t len;
    struct sock_filter insns[1];
};

/* struct seccomp_data, in words: nr, arch, the instruction pointer's two
 * halves and each argument's two. */
struct seccomp_words {
    uint32_t w[SECCOMP_DATA_SIZE / 4];
};

/** Whether the instruction `f` is one seccomp allows, with its operands in
 * range. */
static int insn_allowed(const struct sock_filter *f)
{
    uint32_t code = f->code;

    switch (code) {
    case BPF_LD | BPF_W | BPF_ABS:
        return f->k < SECCOMP_DATA_SIZE && (f->k & 3u) == 0;
    case BPF_LD | BPF_W | BPF_LEN:
    case BPF_LDX | BPF_W | BPF_LEN:
    case BPF_LD | BPF_IMM:
    case BPF_LDX | BPF_IMM:
    case BPF_MISC | BPF_TAX:
    case BPF_MISC | BPF_TXA:
    case BPF_RET | BPF_K:
    case BPF_RET | BPF_A:
    case BPF_ALU | BPF_NEG:
        return 1;
    case BPF_LD | BPF_MEM:
    case BPF_LDX | BPF_MEM:
    case BPF_ST:
    case BPF_STX:
        return f->k < BPF_MEMWORDS;
    case BPF_ALU | BPF_DIV | BPF_K:
    case BPF_ALU | BPF_MOD | BPF_K:
        return f->k != 0;
    default:
        break;
    }
    if (BPF_CLASS(code) == BPF_ALU && (code & ~0xf8u) == BPF_ALU) {
        return BPF_OP(code) <= BPF_XOR && BPF_OP(code) != BPF_NEG;
    }
    if (BPF_CLASS(code) == BPF_JMP && (code & ~0xf8u) == BPF_JMP) {
        return BPF_OP(code) <= BPF_JSET && (BPF_OP(code) != BPF_JA || (code & BPF_X) == 0);
    }
    return 0;
}

/**
 * Whether the program of `len` instructions at `f` may be installed: each
 * instruction allowed, each jump forward and inside the program, and the
 * last a return.
 */
static int program_valid(const struct sock_filter *f, uint32_t len)
{
    uint32_t pc;

    for (pc = 0; pc < len; pc++) {
        uint32_t left = len - pc - 1;

        if (!insn_allowed(&f[pc])) {
            return 0;
        }
        if (BPF_CLASS(f[pc].code) != BPF_JMP) {
            continue;
        }
        if (BPF_OP(f[pc].code) == BPF_JA ? f[pc].k >= left : f[pc].jt >= left || f[pc].jf >= left) {
            return 0;
        }
    }
    return BPF_CLASS(f[len - 1].code) == BPF_RET;
}

/** The result of the arithmetic instruction `code` on `a` and `b`. */
static uint32_t alu(uint32_t code, uint32_t a, uint32_t b, int *divzero)
{
    switch (BPF_OP(code)) {
    case BPF_ADD:
        return a + b;
    case BPF_SUB:
        return a - b;
    case BPF_MUL:
        return a * b;
    case BPF_DIV:
    case BPF_MOD:
        if (b == 0) {
            *divzero = 1;
            return 0;
        }
        return BPF_OP(code) == BPF_DIV ? a / b : a % b;
    case BPF_OR:
        return a | b;
    case BPF_AND:
        return a & b;
    case BPF_LSH:
        return b < 32 ? a << b : 0;
    case BPF_RSH:
        return b < 32 ? a >> b : 0;
    case BPF_NEG:
        return 0u - a;
    default:
        return a ^ b;
    }
}

/** Whether the jump instruction `code` is taken for `a` against `b`. */
static int jump_taken(uint32_t code, uint32_t a, uint32_t b)
{
    switch (BPF_OP(code)) {
    case BPF_JEQ:
        return a == b;
    case BPF_JGT:
        return a > b;
    case BPF_JGE:
        return a >= b;
    default:
        return (a & b) != 0;
    }
}

/**
 * Run the filter `flt` over `d` and return its action. A division by an X
 * of zero ends the program with 0, the kill of the thread, as classic BPF
 * returns 0 for it.
 */
static uint32_t run_filter(const struct seccomp_filter *flt, const struct seccomp_words *d)
{
    uint32_t acc = 0, idx = 0, scratch[BPF_MEMWORDS];
    uint32_t pc = 0;
    int divzero = 0;

    k_memset(scratch, 0, sizeof(scratch));
    while (pc < flt->len) {
        const struct sock_filter *f = &flt->insns[pc++];

        switch (BPF_CLASS(f->code)) {
        case BPF_LD:
            acc = f->code == (BPF_LD | BPF_W | BPF_ABS) ? d->w[f->k / 4]
                  : f->code == (BPF_LD | BPF_MEM)       ? scratch[f->k]
                  : f->code == (BPF_LD | BPF_IMM)       ? f->k
                                                        : SECCOMP_DATA_SIZE;
            break;
        case BPF_LDX:
            idx = f->code == (BPF_LDX | BPF_MEM)   ? scratch[f->k]
                  : f->code == (BPF_LDX | BPF_IMM) ? f->k
                                                   : SECCOMP_DATA_SIZE;
            break;
        case BPF_ST:
            scratch[f->k] = acc;
            break;
        case BPF_STX:
            scratch[f->k] = idx;
            break;
        case BPF_ALU:
            acc = alu(f->code, acc, (f->code & BPF_X) ? idx : f->k, &divzero);
            if (divzero) {
                return SECCOMP_RET_KILL_THREAD;
            }
            break;
        case BPF_JMP:
            if (BPF_OP(f->code) == BPF_JA) {
                pc += f->k;
            } else {
                pc += jump_taken(f->code, acc, (f->code & BPF_X) ? idx : f->k) ? f->jt : f->jf;
            }
            break;
        case BPF_RET:
            return (f->code & BPF_A) ? acc : f->k;
        default:
            if (f->code & BPF_TXA) {
                acc = idx;
            } else {
                idx = acc;
            }
            break;
        }
    }
    return SECCOMP_RET_KILL_THREAD;
}

/** The action's precedence: the lower, the stronger, as Linux compares the
 * actions as signed numbers. */
static int32_t precedence(uint32_t ret)
{
    return (int32_t)(ret & SECCOMP_RET_ACTION_FULL);
}

/** Store `v` in the two words of `d` from `at`, low word first, as a
 * little-endian `seccomp_data` has a 64-bit field. */
static void put_u64(struct seccomp_words *d, int at, uint64_t v)
{
    d->w[at] = (uint32_t)v;
    d->w[at + 1] = (uint32_t)(v >> 32);
}

/** The action of `p`'s chain for the call in `tf`: every filter's, the one
 * of highest precedence. */
static uint32_t chain_action(const process_t *p, const isr_params_t *tf, uint32_t nr)
{
    const struct seccomp_filter *f;
    struct seccomp_words d;
    uint32_t ret = SECCOMP_RET_ALLOW;

    k_memset(&d, 0, sizeof(d));
    d.w[0] = nr;
    d.w[1] = AUDIT_ARCH;
    put_u64(&d, 2, (uint64_t)FRAME_PC(tf));
    put_u64(&d, 4, (uint64_t)SC_ARG(tf, 0));
    put_u64(&d, 6, (uint64_t)SC_ARG(tf, 1));
    put_u64(&d, 8, (uint64_t)SC_ARG(tf, 2));
    put_u64(&d, 10, (uint64_t)SC_ARG(tf, 3));
    put_u64(&d, 12, (uint64_t)SC_ARG(tf, 4));
    put_u64(&d, 14, (uint64_t)SC_ARG(tf, 5));
    for (f = p->seccomp; f; f = f->prev) {
        uint32_t cur = run_filter(f, &d);

        if (precedence(cur) < precedence(ret)) {
            ret = cur;
        }
    }
    return ret;
}

/**
 * End the current thread, or its whole process, for a call its filters
 * kill, as killed by SIGSYS: the process with a core where it may leave
 * one, a thread that is not the last alone. Does not return.
 */
static void seccomp_kill(uint32_t action, uint32_t nr)
{
    klog(KLOG_INFO, "seccomp", "pid %d: call %d killed", (int)current_proc->pid, (int)nr);
    if (action == SECCOMP_RET_KILL_THREAD && current_proc->tg->live > 1) {
        exit_current(SIGSYS);
    }
    terminate_current_process(SIGSYS | WCOREFLAG);
}

/** Post SIGSYS for the call `nr` a filter trapped with `data`, and put the
 * result register back at the call's number, as Linux's rollback does, for
 * the handler. */
static void seccomp_trap(isr_params_t *tf, uint32_t nr, uint32_t data)
{
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = SYS_SECCOMP;
    info.pid = (uint32_t)FRAME_PC(tf);
    info.uid = nr;
    info.status = (int32_t)AUDIT_ARCH;
    info.err = (int32_t)data;
    SC_RET(tf) = nr;
    signal_force(current_proc, SIGSYS, &info);
}

/**
 * The tracer's turn for a call a filter answered with SECCOMP_RET_TRACE:
 * with PTRACE_O_TRACESECCOMP, a PTRACE_EVENT_SECCOMP stop with the data,
 * after which the call runs unless the tracer skipped it or the filters,
 * run again on what the tracer left, refuse it. Without a tracer that asked,
 * the call fails with ENOSYS. Returns 1 to run the call.
 */
static int seccomp_trace(isr_params_t *tf, uint32_t nr, uint32_t data, long *ret)
{
    process_t *self = current_proc;
    uint32_t again;

    if (!(self->ptrace & PT_TRACED) || !(self->ptrace_opts & (1u << PTRACE_EVENT_SECCOMP))) {
        *ret = -ENOSYS;
        return 0;
    }
    self->orig_nr = (int32_t)nr;
    SC_RET(tf) = (unsigned long)-ENOSYS;
    ptrace_event(PTRACE_EVENT_SECCOMP, data);
    if (self->orig_nr == -1 || ((self->sig_pending | self->tg->pending) & KSIG_BIT(SIGKILL))) {
        *ret = (long)SC_RET(tf);
        return 0;
    }
    again = chain_action(self, tf, (uint32_t)self->orig_nr);
    if ((again & SECCOMP_RET_ACTION_FULL) == SECCOMP_RET_TRACE ||
        (again & SECCOMP_RET_ACTION_FULL) == SECCOMP_RET_ALLOW ||
        (again & SECCOMP_RET_ACTION_FULL) == SECCOMP_RET_LOG) {
        return 1;
    }
    return seccomp_permit(tf, (uint32_t)self->orig_nr, ret);
}

int seccomp_permit(isr_params_t *tf, uint32_t nr, long *ret)
{
    process_t *self = current_proc;
    uint32_t action, data;

    if (self->seccomp_mode == SECCOMP_MODE_STRICT) {
        if (nr == NR_READ || nr == NR_WRITE || nr == NR_EXIT || nr == NR_SIGRETURN) {
            return 1;
        }
        klog(KLOG_INFO, "seccomp", "pid %d: call %d outside the strict mode", (int)self->pid,
             (int)nr);
        terminate_current_process(SIGKILL);
        return 0;
    }
    action = chain_action(self, tf, nr);
    data = action & SECCOMP_RET_DATA;
    switch (action & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_ALLOW:
    case SECCOMP_RET_LOG:
        return 1;
    case SECCOMP_RET_ERRNO:
        *ret = -(int32_t)(data > MAX_ERRNO ? MAX_ERRNO : data);
        return 0;
    case SECCOMP_RET_TRAP:
        seccomp_trap(tf, nr, data);
        *ret = (int32_t)nr;
        return 0;
    case SECCOMP_RET_TRACE:
        return seccomp_trace(tf, nr, data, ret);
    case SECCOMP_RET_USER_NOTIF:
        *ret = -ENOSYS;
        return 0;
    case SECCOMP_RET_KILL_THREAD:
        seccomp_kill(SECCOMP_RET_KILL_THREAD, nr);
        return 0;
    default:
        seccomp_kill(SECCOMP_RET_KILL_PROCESS, nr);
        return 0;
    }
}

/** Drop one reference to the chain from `f`, freeing each filter no one
 * else holds. */
static void chain_put(struct seccomp_filter *f)
{
    while (f && --f->refs == 0) {
        struct seccomp_filter *prev = f->prev;

        kfree(f);
        f = prev;
    }
}

void seccomp_inherit(process_t *child, const process_t *parent)
{
    child->seccomp_mode = parent->seccomp_mode;
    child->seccomp = parent->seccomp;
    if (child->seccomp) {
        child->seccomp->refs++;
    }
}

void seccomp_release(process_t *p)
{
    chain_put(p->seccomp);
    p->seccomp = NULL;
}

uint32_t seccomp_filter_count(const process_t *p)
{
    const struct seccomp_filter *f;
    uint32_t n = 0;

    for (f = p->seccomp; f; f = f->prev) {
        n++;
    }
    return n;
}

/**
 * The filter `struct sock_fprog` at the user address `uprog` describes,
 * checked and copied: its length and the program's address, then the
 * program. NULL with `*err` set: -EFAULT, -EINVAL for an empty, long or
 * refused program, or -ENOMEM.
 */
static struct seccomp_filter *filter_from_user(uint32_t uprog, int32_t *err)
{
    uint32_t words[2]; /* struct sock_fprog: a 16-bit length, then the address */
    struct seccomp_filter *f;
    uint32_t len;

    if (copy_from_user(words, (const void __user *)uprog, sizeof(words)) != 0) {
        *err = -EFAULT;
        return NULL;
    }
    len = words[0] & 0xFFFFu;
    if (len == 0 || len > BPF_MAXINSNS) {
        *err = -EINVAL;
        return NULL;
    }
    f = kmalloc(sizeof(*f) + (len - 1u) * sizeof(struct sock_filter));
    if (!f) {
        *err = -ENOMEM;
        return NULL;
    }
    f->refs = 1;
    f->prev = NULL;
    f->len = len;
    if (copy_from_user(f->insns, (const void __user *)words[1], len * sizeof(f->insns[0])) != 0) {
        *err = -EFAULT;
    } else if (!program_valid(f->insns, f->len)) {
        *err = -EINVAL;
    } else {
        return f;
    }
    kfree(f);
    return NULL;
}

/** Whether the chain of `f` with `extra` more instructions stays within
 * Linux's limit on a path, each filter counted with four more. */
static int chain_fits(const struct seccomp_filter *f, uint32_t extra)
{
    uint32_t total = extra + 4u;

    for (; f; f = f->prev) {
        total += f->len + 4u;
    }
    return total <= MAX_INSNS_PER_PATH;
}

/** Whether `f` is the chain `chain` or one of its older filters. */
static int chain_has(const struct seccomp_filter *chain, const struct seccomp_filter *f)
{
    for (; chain; chain = chain->prev) {
        if (chain == f) {
            return 1;
        }
    }
    return f == NULL;
}

/**
 * For SECCOMP_FILTER_FLAG_TSYNC: the id of a thread of the caller's process
 * that cannot take the caller's chain, one in the strict mode or with a
 * filter the caller lacks; 0 for none.
 */
static uint32_t tsync_blocker(const process_t *self)
{
    const process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (t == self || t->tg != self->tg || t->state == PROC_ZOMBIE) {
            continue;
        }
        if (t->seccomp_mode == SECCOMP_MODE_STRICT || !chain_has(self->seccomp, t->seccomp)) {
            return t->pid;
        }
    }
    return 0;
}

/** Give every other thread of the caller's process the caller's chain, its
 * mode, and its no_new_privs when the caller has it set. */
static void tsync_apply(process_t *self)
{
    process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (t == self || t->tg != self->tg || t->state == PROC_ZOMBIE) {
            continue;
        }
        self->seccomp->refs++;
        chain_put(t->seccomp);
        t->seccomp = self->seccomp;
        t->seccomp_mode = SECCOMP_MODE_FILTER;
        if (self->no_new_privs) {
            t->no_new_privs = 1;
        }
    }
}

/**
 * Add the filter at the user address `uprog` to the current thread, with
 * `flags`: a caller without no_new_privs must be privileged, and under
 * TSYNC every thread of the process takes the new chain, unless one cannot,
 * whose id is returned, or -ESRCH with TSYNC_ESRCH.
 */
static int32_t set_mode_filter(uint32_t flags, uint32_t uprog)
{
    process_t *self = current_proc;
    struct seccomp_filter *f;
    uint32_t blocker;
    int32_t err = 0;

    if ((flags & ~SECCOMP_FILTER_FLAGS) != 0 || self->seccomp_mode == SECCOMP_MODE_STRICT) {
        return -EINVAL;
    }
    if (!self->no_new_privs && !proc_privileged(self)) {
        return -EACCES;
    }
    f = filter_from_user(uprog, &err);
    if (!f) {
        return err;
    }
    if (!chain_fits(self->seccomp, f->len)) {
        kfree(f);
        return -ENOMEM;
    }
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        blocker = tsync_blocker(self);
        if (blocker != 0) {
            kfree(f);
            return (flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH) ? -ESRCH : (int32_t)blocker;
        }
    }
    f->prev = self->seccomp;
    self->seccomp = f;
    self->seccomp_mode = SECCOMP_MODE_FILTER;
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        tsync_apply(self);
    }
    return 0;
}

/** Whether `action` is one the filters may return. */
static int action_available(uint32_t action)
{
    switch (action) {
    case SECCOMP_RET_KILL_PROCESS:
    case SECCOMP_RET_KILL_THREAD:
    case SECCOMP_RET_TRAP:
    case SECCOMP_RET_ERRNO:
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_LOG:
    case SECCOMP_RET_ALLOW:
        return 1;
    default:
        return 0;
    }
}

int32_t seccomp_prctl_set(uint32_t mode, uint32_t prog)
{
    if (mode == SECCOMP_MODE_STRICT) {
        if (current_proc->seccomp_mode == SECCOMP_MODE_FILTER) {
            return -EINVAL;
        }
        current_proc->seccomp_mode = SECCOMP_MODE_STRICT;
        return 0;
    }
    if (mode == SECCOMP_MODE_FILTER) {
        return set_mode_filter(0, prog);
    }
    return -EINVAL;
}

/**
 * seccomp (354): EBX the operation, ECX its flags, EDX its argument.
 *
 * SECCOMP_SET_MODE_STRICT, with no flags and no argument, leaves the thread
 * only read, write, exit and sigreturn; any other call kills it.
 * SECCOMP_SET_MODE_FILTER adds the filter `struct sock_fprog` describes.
 * SECCOMP_GET_ACTION_AVAIL returns 0 for an action the filters may return
 * and -EOPNOTSUPP for another, and SECCOMP_GET_NOTIF_SIZES stores Linux's
 * three sizes. SECCOMP_RET_USER_NOTIF is not one of the actions, since a
 * filter cannot be installed with a listener (SECCOMP_FILTER_FLAG_NEW_LISTENER
 * is refused with -EINVAL), and a filter that returns it fails the call with
 * ENOSYS, as Linux does without a listener.
 */
long sys_seccomp(isr_params_t *params)
{
    uint32_t op = params->ebx, flags = params->ecx, arg = params->edx;
    uint32_t action;
    uint16_t sizes[3] = {80, 24, SECCOMP_DATA_SIZE};

    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags != 0 || arg != 0) {
            return -EINVAL;
        }
        return seccomp_prctl_set(SECCOMP_MODE_STRICT, 0);
    case SECCOMP_SET_MODE_FILTER:
        return set_mode_filter(flags, arg);
    case SECCOMP_GET_ACTION_AVAIL:
        if (flags != 0) {
            return -EINVAL;
        }
        if (copy_from_user(&action, (const void __user *)arg, sizeof(action)) != 0) {
            return -EFAULT;
        }
        return action_available(action) ? 0 : -EOPNOTSUPP;
    case SECCOMP_GET_NOTIF_SIZES:
        if (flags != 0) {
            return -EINVAL;
        }
        return copy_to_user((void __user *)arg, sizes, sizeof(sizes)) != 0 ? -EFAULT : 0;
    default:
        return -EINVAL;
    }
}
