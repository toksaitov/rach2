/*
 * process_vm.c: process_vm_readv(2) and process_vm_writev(2), copies between
 * the caller's memory and another process's, as Linux 6.12's
 * mm/process_vm_access.c makes them: under ptrace's rules for attaching,
 * without the force a tracer's access has, so that a page the target may not
 * read or write is refused. strace reads a traced program's strings with
 * them.
 */

#include "process_vm.h"

#include "ns.h"
#include "percpu.h"
#include "process.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../mm/vm.h"
#include "../syscall.h"

/* The vectors one call takes on each side, Linux's UIO_MAXIOV. */
#define PVM_IOV_MAX 1024u

/* The most bytes one call moves, Linux's MAX_RW_COUNT: INT_MAX less a
 * page, so that the count fits the return value. */
#define PVM_MAX_RW 0x7FFFF000ul

/* The bytes moved through the kernel at a time. */
#define PVM_CHUNK 512u

/* One buffer of a vector, its address and length. */
struct pvm_vec {
    uintptr_t base;
    uintptr_t len;
};

/**
 * Read the `cnt` buffers of the vector at `uvec` into `*out`, which the
 * caller frees, NULL for none, as Linux's iovec_from_user reads them: in the
 * caller's layout, a struct iovec of two 32-bit words from an i386 caller.
 * More than PVM_IOV_MAX is -EINVAL, a vector that cannot be read -EFAULT, a
 * length that is negative as a signed size -EINVAL, and -ENOMEM when the
 * heap has no room.
 */
static int32_t pvm_import(uintptr_t uvec, unsigned long cnt, struct pvm_vec **out)
{
    int compat = in_compat_syscall();
    unsigned long step = sizeof(struct pvm_vec);
    struct pvm_vec *v;
    unsigned long i;

    if (compat) {
        step = 2u * sizeof(uint32_t);
    }
    *out = NULL;
    if (cnt > PVM_IOV_MAX) {
        return -EINVAL;
    }
    if (cnt == 0) {
        return 0;
    }
    v = kmalloc(cnt * sizeof(*v));
    if (!v) {
        return -ENOMEM;
    }
    for (i = 0; i < cnt; i++) {
        const void __user *at = (const void __user *)(uvec + i * step);
        uint32_t w[2];
        int bad;

        if (compat) {
            bad = copy_from_user(w, at, sizeof(w)) != 0;
            v[i].base = w[0];
            v[i].len = w[1];
        } else {
            bad = copy_from_user(&v[i], at, sizeof(v[i])) != 0;
        }
        if (bad) {
            kfree(v);
            return -EFAULT;
        }
        if (compat ? (int32_t)v[i].len < 0 : (long)v[i].len < 0) {
            kfree(v);
            return -EINVAL;
        }
    }
    *out = v;
    return 0;
}

/**
 * Check the caller's own buffers as Linux's import_iovec does, each in turn:
 * one that runs past user space is -EFAULT, and the lengths are cut so that
 * together they are at most PVM_MAX_RW. Returns the total.
 */
static long pvm_local_total(struct pvm_vec *v, unsigned long cnt)
{
    uintptr_t total = 0;
    unsigned long i;

    for (i = 0; i < cnt; i++) {
        if (v[i].base > USER_ADDR_MAX || v[i].len > USER_ADDR_MAX - v[i].base) {
            return -EFAULT;
        }
        if (v[i].len > PVM_MAX_RW - total) {
            v[i].len = PVM_MAX_RW - total;
        }
        total += v[i].len;
    }
    return (long)total;
}

/**
 * Whether the caller may reach the memory of `t`, as Linux's mm_access with
 * PTRACE_MODE_ATTACH_REALCREDS allows it: its own address space, its own
 * thread group, a privileged caller, or a caller whose real user and group
 * IDs are each of `t`'s three, with `t` dumpable and not privileged.
 */
static int pvm_may_access(const process_t *t)
{
    if (t->mm == current_proc->mm || t->tg == current_proc->tg || proc_privileged(current_proc)) {
        return 1;
    }
    return current_proc->ruid == t->ruid && current_proc->ruid == t->euid &&
           current_proc->ruid == t->suid && current_proc->rgid == t->rgid &&
           current_proc->rgid == t->egid && current_proc->rgid == t->sgid && t->mm->dumpable &&
           !proc_privileged(t);
}

/**
 * Move `n` bytes, within one page on each side, between the caller's `laddr`
 * and `raddr` of `mm`: a read into the caller with `write` 0, a write into
 * `mm` otherwise. The remote page must lie in a region that allows the
 * access itself, since the copy has no tracer's force; one that does not,
 * and one `paging_access` cannot reach, a guard or device memory, are
 * -EFAULT, as is a fault in the caller's buffer. 0 or -errno.
 */
static int32_t pvm_move(struct mm *mm, uintptr_t laddr, uintptr_t raddr, uint32_t n, int write)
{
    const vma_t *vma = vm_find(&mm->vm, raddr);
    unsigned char buf[PVM_CHUNK];
    int32_t rc;

    if (!vma || !(vma->prot & (write ? VM_PROT_WRITE : VM_PROT_READ))) {
        return -EFAULT;
    }
    if (write) {
        if (copy_from_user(buf, (const void __user *)laddr, n) != 0) {
            return -EFAULT;
        }
        rc = paging_access(mm, raddr, buf, n, 1);
        return rc == -EIO ? -EFAULT : rc;
    }
    rc = paging_access(mm, raddr, buf, n, 0);
    if (rc != 0) {
        return rc == -EIO ? -EFAULT : rc;
    }
    return copy_to_user((void __user *)laddr, buf, n) != 0 ? -EFAULT : 0;
}

/** The least of `a` and `b`. */
static uintptr_t pvm_min(uintptr_t a, uintptr_t b)
{
    return a < b ? a : b;
}

/**
 * Move the remote buffers `r` of `mm`, in order, to or from the local ones
 * `l`, until either side ends or a page fails. Pieces stop at each page of
 * either side, so that a failure leaves every byte before its page moved, as
 * Linux's pinning of whole pages does. Returns the bytes moved, or the
 * error when none were.
 */
static long pvm_copy(struct mm *mm, const struct pvm_vec *l, unsigned long lcnt,
                     const struct pvm_vec *r, unsigned long rcnt, int write)
{
    unsigned long li = 0, ri;
    uintptr_t loff = 0;
    long done = 0;
    int32_t rc = 0;

    for (ri = 0; ri < rcnt && rc == 0; ri++) {
        uintptr_t raddr = r[ri].base;
        uintptr_t rlen = r[ri].len;

        while (rlen > 0 && rc == 0) {
            uintptr_t laddr, n;

            while (li < lcnt && loff == l[li].len) {
                li++;
                loff = 0;
            }
            if (li == lcnt) {
                return done;
            }
            laddr = l[li].base + loff;
            n = pvm_min(rlen, l[li].len - loff);
            n = pvm_min(n, PAGE_SIZE - (raddr & (PAGE_SIZE - 1u)));
            n = pvm_min(n, PAGE_SIZE - (laddr & (PAGE_SIZE - 1u)));
            n = pvm_min(n, PVM_CHUNK);
            rc = pvm_move(mm, laddr, raddr, (uint32_t)n, write);
            if (rc == 0) {
                done += (long)n;
                loff += n;
                raddr += n;
                rlen -= n;
            }
        }
    }
    return done > 0 ? done : rc;
}

/**
 * The call, in Linux 6.12's order of checks: flags other than 0 are
 * -EINVAL; the local vector is read and checked (`pvm_import`,
 * `pvm_local_total`), and with no byte in it the answer is 0; the remote
 * vector is read, and with no byte in it the answer is 0, before the
 * process is looked for; a `pid` that names no process or thread with an
 * address space is -ESRCH, and one the caller may not reach
 * (`pvm_may_access`) -EPERM. The address space is held while the copy may
 * sleep, so that the target's end leaves its pages until the copy is done.
 */
static long process_vm_rw(isr_params_t *params, int write)
{
    struct pvm_vec *l = NULL, *r = NULL;
    unsigned long lcnt = SC_ARG(params, 2);
    unsigned long rcnt = SC_ARG(params, 4);
    unsigned long i;
    uintptr_t rtotal = 0;
    process_t *t;
    struct mm *mm;
    long rc;

    if (SC_ARG(params, 5) != 0) {
        return -EINVAL;
    }
    rc = pvm_import(SC_ARG(params, 1), lcnt, &l);
    if (rc == 0) {
        rc = pvm_local_total(l, lcnt);
    }
    if (rc <= 0) {
        kfree(l);
        return rc;
    }
    rc = pvm_import(SC_ARG(params, 3), rcnt, &r);
    for (i = 0; rc == 0 && i < rcnt; i++) {
        rtotal |= r[i].len;
    }
    if (rc != 0 || rtotal == 0) {
        kfree(l);
        kfree(r);
        return rc;
    }
    t = proc_named((uint32_t)SC_ARG(params, 0));
    mm = t ? t->mm : NULL;
    if (!mm) {
        rc = -ESRCH;
    } else if (!pvm_may_access(t)) {
        rc = -EPERM;
    } else {
        mm_get(mm);
        rc = pvm_copy(mm, l, lcnt, r, rcnt, write);
        mm_put(mm);
    }
    kfree(l);
    kfree(r);
    return rc;
}

long sys_process_vm_readv(isr_params_t *params)
{
    return process_vm_rw(params, 0);
}

long sys_process_vm_writev(isr_params_t *params)
{
    return process_vm_rw(params, 1);
}
