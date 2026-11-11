/*
 * rseq.c: rseq(2), restartable sequences, as Linux 6.12's kernel/rseq.c keeps
 * them. A thread registers an area of its memory, a struct rseq, in which the
 * kernel writes the processor the thread runs on and the thread names the
 * critical section it is in; a switch away from the processor or a signal
 * while the thread is inside the section sends it to the section's abort
 * handler. glibc registers every thread, and its sched_getcpu reads the area.
 */

#include "rseq.h"

#include "percpu.h"
#include "process.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

/* The offsets of struct rseq's fields, Linux's <linux/rseq.h>: the
 * processor at the start of the section, the processor, the address of the
 * section's struct rseq_cs, the thread's flags, the memory node and the
 * concurrency ID. */
#define RSEQ_CPU_ID_START 0u
#define RSEQ_CS           8u
#define RSEQ_FLAGS        16u
#define RSEQ_NODE_ID      20u

/* The size of the first ABI's area, the least rseq_len there is. */
#define RSEQ_ORIG_SIZE 32u

#define RSEQ_FLAG_UNREGISTER 1

/* What cpu_id reads while the thread has no area registered. */
#define RSEQ_CPU_ID_UNINITIALIZED 0xFFFFFFFFu

/* A critical section, Linux's struct rseq_cs: the version, 0; the flags,
 * which must be 0; the first instruction; the bytes from it to the
 * instruction past the commit; and the abort handler, which the signature
 * the thread registered precedes. */
struct rseq_cs {
    uint32_t version;
    uint32_t flags;
    uint64_t start_ip;
    uint64_t post_commit_offset;
    uint64_t abort_ip;
};

/** The user address `field` bytes into the area of `p`. */
static void __user *rseq_at(const process_t *p, uint32_t field)
{
    return (void __user *)(p->rseq + field);
}

/**
 * Write the processor into the area of `p`, the current thread: cpu_id_start
 * and cpu_id, node_id 0 on a machine of one memory node, and mm_cid, which
 * is the processor's number here: unique among the threads of an address
 * space that run at once and below the number of processors, as Linux's
 * concurrency ID is, though not packed below the number of threads. 0 or
 * -EFAULT.
 */
static int rseq_update(const process_t *p)
{
    uint32_t ids[2];

    ids[0] = arch_cpu_id();
    ids[1] = ids[0];
    if (copy_to_user(rseq_at(p, RSEQ_CPU_ID_START), ids, sizeof(ids)) != 0) {
        return -EFAULT;
    }
    ids[0] = 0;
    return copy_to_user(rseq_at(p, RSEQ_NODE_ID), ids, sizeof(ids)) != 0 ? -EFAULT : 0;
}

/** Clear the area's rseq_cs, as the thread leaves its section; 0 or
 * -EFAULT. */
static int rseq_clear_cs(const process_t *p)
{
    uint64_t zero = 0;

    return copy_to_user(rseq_at(p, RSEQ_CS), &zero, sizeof(zero)) != 0 ? -EFAULT : 0;
}

/**
 * Read the section the area of `p` names into `*cs`, all zeros for none,
 * with Linux's checks: -EFAULT when it cannot be read, -EINVAL when it is
 * above user space, runs past it or wraps, has a version, has its abort
 * handler inside it, or the four bytes before the abort handler are not
 * the signature `p` registered.
 */
static int rseq_get_cs(const process_t *p, struct rseq_cs *cs)
{
    uint64_t ptr;
    uint32_t sig;

    if (copy_from_user(&ptr, rseq_at(p, RSEQ_CS), sizeof(ptr)) != 0) {
        return -EFAULT;
    }
    if (ptr == 0) {
        k_memset(cs, 0, sizeof(*cs));
        return 0;
    }
    if (ptr >= USER_ADDR_MAX) {
        return -EINVAL;
    }
    if (copy_from_user(cs, (const void __user *)(uintptr_t)ptr, sizeof(*cs)) != 0) {
        return -EFAULT;
    }
    if (cs->start_ip >= USER_ADDR_MAX || cs->start_ip + cs->post_commit_offset >= USER_ADDR_MAX ||
        cs->abort_ip >= USER_ADDR_MAX || cs->version > 0 ||
        cs->start_ip + cs->post_commit_offset < cs->start_ip ||
        cs->abort_ip - cs->start_ip < cs->post_commit_offset) {
        return -EINVAL;
    }
    if (copy_from_user(&sig, (const void __user *)(uintptr_t)(cs->abort_ip - sizeof(sig)),
                       sizeof(sig)) != 0) {
        return -EFAULT;
    }
    return sig == p->rseq_sig ? 0 : -EINVAL;
}

/**
 * Send `p`, the current thread returning to the frame `params`, to the abort
 * handler of the section it is in when an event since the last look asks for
 * it, as Linux's rseq_ip_fixup: outside a section the area's rseq_cs is
 * cleared; inside one, flags on the section or on the area are -EINVAL, as
 * Linux refuses every flag since 6.0, and the events are taken; with any,
 * rseq_cs is cleared and the frame resumes at the abort handler. 0 or
 * -errno.
 */
static int rseq_fixup(process_t *p, isr_params_t *params)
{
    struct rseq_cs cs;
    uint64_t ip = (uint64_t)FRAME_PC(params);
    uint32_t flags, event;
    int rc;

    rc = rseq_get_cs(p, &cs);
    if (rc != 0) {
        return rc;
    }
    if (ip - cs.start_ip >= cs.post_commit_offset) {
        return rseq_clear_cs(p);
    }
    if (cs.flags != 0) {
        return -EINVAL;
    }
    if (copy_from_user(&flags, rseq_at(p, RSEQ_FLAGS), sizeof(flags)) != 0) {
        return -EFAULT;
    }
    if (flags != 0) {
        return -EINVAL;
    }
    event = p->rseq_event;
    p->rseq_event = 0;
    if (event == 0) {
        return 0;
    }
    rc = rseq_clear_cs(p);
    if (rc != 0) {
        return rc;
    }
    FRAME_PC(params) = (uintptr_t)cs.abort_ip;
    return 0;
}

/** Post the current thread SIGSEGV for an area that failed during the
 * delivery of `signo`, 0 for none, as Linux's force_sigsegv: with `signo`
 * SIGSEGV its handler is reset first, so that it cannot loop. */
static void rseq_fail(int signo)
{
    struct k_siginfo info;

    if (signo == SIGSEGV) {
        current_proc->sigacts[SIGSEGV].handler = KSIG_DFL;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    signal_force(current_proc, SIGSEGV, &info);
}

void rseq_preempt(process_t *p)
{
    if (p->rseq != 0) {
        p->rseq_event |= RSEQ_EVENT_PREEMPT;
        p->rseq_notify = 1;
    }
}

void rseq_migrate(process_t *p)
{
    if (p->rseq != 0) {
        p->rseq_event |= RSEQ_EVENT_MIGRATE;
        p->rseq_notify = 1;
    }
}

/**
 * The look repeats while it is asked for again, since a write to the area
 * may sleep bringing its page in, and the switch marks the thread anew. A
 * failure's SIGSEGV is delivered here (`deliver_signals`), not at the next
 * entry to the kernel, which a thread in a loop may make only at the next
 * tick.
 */
void rseq_return(isr_params_t *params)
{
    process_t *p = current_proc;

    while (p->rseq_notify) {
        p->rseq_notify = 0;
        if (p->rseq != 0 && (rseq_fixup(p, params) != 0 || rseq_update(p) != 0)) {
            rseq_fail(0);
            deliver_signals(params);
        }
    }
}

void rseq_signal(isr_params_t *params, int signo)
{
    process_t *p = current_proc;

    if (p->rseq == 0) {
        return;
    }
    p->rseq_event |= RSEQ_EVENT_SIGNAL;
    if (rseq_fixup(p, params) != 0 || rseq_update(p) != 0) {
        rseq_fail(signo);
    }
}

void rseq_fork(process_t *child, const process_t *parent, int shares_vm)
{
    if (shares_vm || parent->rseq == 0) {
        return;
    }
    child->rseq = parent->rseq;
    child->rseq_len = parent->rseq_len;
    child->rseq_sig = parent->rseq_sig;
    child->rseq_event = parent->rseq_event;
    child->rseq_notify = 1;
}

void rseq_exec(process_t *p)
{
    p->rseq = 0;
    p->rseq_len = 0;
    p->rseq_sig = 0;
    p->rseq_event = 0;
    p->rseq_notify = 0;
}

/** Put the area of `p` back as an unregistered one reads: cpu_id_start 0,
 * cpu_id RSEQ_CPU_ID_UNINITIALIZED, node_id 0 and mm_cid 0. 0 or
 * -EFAULT. */
static int rseq_reset(const process_t *p)
{
    uint32_t ids[2];

    ids[0] = 0;
    ids[1] = RSEQ_CPU_ID_UNINITIALIZED;
    if (copy_to_user(rseq_at(p, RSEQ_CPU_ID_START), ids, sizeof(ids)) != 0) {
        return -EFAULT;
    }
    ids[1] = 0;
    return copy_to_user(rseq_at(p, RSEQ_NODE_ID), ids, sizeof(ids)) != 0 ? -EFAULT : 0;
}

/**
 * Register or unregister the calling thread's area, with Linux 6.12's
 * checks and errors. RSEQ_FLAG_UNREGISTER, alone, takes the area, length
 * and signature registered: another area or length is -EINVAL, another
 * signature -EPERM; the area is put back as an unregistered one reads, or
 * -EFAULT. Any other flag is -EINVAL. With an area registered, a second
 * registration is -EINVAL for another area or length, -EPERM for another
 * signature and -EBUSY for the same. A new area must be at least 32 bytes,
 * aligned to 32, else -EINVAL, and lie below the top of user space, else
 * -EFAULT; its fields are written at the return to user mode, where an area
 * that cannot be written is SIGSEGV. An area at address 0 registers none,
 * as on Linux.
 */
long sys_rseq(isr_params_t *params)
{
    process_t *p = current_proc;
    uintptr_t area = SC_ARG(params, 0);
    uint32_t len = (uint32_t)SC_ARG(params, 1);
    int flags = (int)SC_ARG(params, 2);
    uint32_t sig = (uint32_t)SC_ARG(params, 3);

    if (flags & RSEQ_FLAG_UNREGISTER) {
        if ((flags & ~RSEQ_FLAG_UNREGISTER) != 0 || p->rseq == 0 || p->rseq != area ||
            len != p->rseq_len) {
            return -EINVAL;
        }
        if (sig != p->rseq_sig) {
            return -EPERM;
        }
        if (rseq_reset(p) != 0) {
            return -EFAULT;
        }
        rseq_exec(p);
        return 0;
    }
    if (flags != 0) {
        return -EINVAL;
    }
    if (p->rseq != 0) {
        if (p->rseq != area || len != p->rseq_len) {
            return -EINVAL;
        }
        return sig != p->rseq_sig ? -EPERM : -EBUSY;
    }
    if (len < RSEQ_ORIG_SIZE || (area & (RSEQ_ALIGN - 1u)) != 0) {
        return -EINVAL;
    }
    if (area > USER_ADDR_MAX || (uintptr_t)len > USER_ADDR_MAX - area) {
        return -EFAULT;
    }
    p->rseq = area;
    p->rseq_len = len;
    p->rseq_sig = sig;
    p->rseq_notify = area != 0;
    return 0;
}
