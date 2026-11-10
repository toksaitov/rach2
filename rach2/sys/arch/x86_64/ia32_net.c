/*
 * ia32_net.c: the i386 forms of the socket calls for the compat table
 * (ia32_systab.c): socketcall, and the i386 msghdr and control message
 * header the shared socket code reads and writes for an i386 call.
 */

#include "ia32.h"
#include "ia32_abi.h"

#include "../arch.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../mm/usercopy.h"
#include "../../network/socket/socket.h"
#include "../../proc/process.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

#if RACH2_IA32

/*
 * An i386 call runs the shared socket code, as on Linux, whose message
 * calls pick i386's structures while `in_compat_syscall` is true: the
 * vectors through `iov_fetch`, the headers and control messages through
 * the functions below (`msghdr_get`, `cmsg_get`), the mmsghdr's stride,
 * the recvmmsg timeout and the SO_RCVTIMEO and SO_SNDTIMEO values in
 * syscall_net.c. Only socketcall needs a handler of its own here, for its
 * array of 32-bit words.
 */

/**
 * Read the i386 message header at user address `u` into `h`.
 * The caller holds the big lock; the copy may sleep. Returns 0 or -EFAULT.
 */
int ia32_msghdr_get(struct kernel_msghdr *h, uintptr_t u)
{
    struct ia32_msghdr m;

    if (copy_from_user(&m, (const void __user *)u, sizeof(m)) != 0) {
        return -EFAULT;
    }
    h->msg_name = m.msg_name;
    h->msg_namelen = m.msg_namelen;
    h->msg_iov = m.msg_iov;
    h->msg_iovlen = m.msg_iovlen;
    h->msg_control = m.msg_control;
    h->msg_controllen = m.msg_controllen;
    h->msg_flags = m.msg_flags;
    return 0;
}

/**
 * Write the received lengths and flags of `h` to the i386 header at `u`.
 * Leave its pointers unchanged. The caller holds the big lock; the copies
 * may sleep. Returns 0 or -EFAULT, possibly after a partial write.
 */
int ia32_msghdr_put(uintptr_t u, const struct kernel_msghdr *h)
{
    int32_t namelen = h->msg_namelen;
    uint32_t controllen = (uint32_t)h->msg_controllen;
    uint32_t flags = h->msg_flags;

    if ((h->msg_name && copy_to_user((void __user *)(u + offsetof(struct ia32_msghdr, msg_namelen)),
                                     &namelen, sizeof(namelen)) != 0) ||
        copy_to_user((void __user *)(u + offsetof(struct ia32_msghdr, msg_controllen)), &controllen,
                     sizeof(controllen)) != 0 ||
        copy_to_user((void __user *)(u + offsetof(struct ia32_msghdr, msg_flags)), &flags,
                     sizeof(flags)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Read the i386 control header at user address `at` into `h`.
 * The caller holds the big lock; the copy may sleep. Returns 0 or -EFAULT.
 */
int ia32_cmsg_get(struct kernel_cmsghdr *h, uintptr_t at)
{
    struct ia32_cmsghdr c;

    if (copy_from_user(&c, (const void __user *)at, sizeof(c)) != 0) {
        return -EFAULT;
    }
    h->cmsg_len = c.cmsg_len;
    h->cmsg_level = c.cmsg_level;
    h->cmsg_type = c.cmsg_type;
    return 0;
}

/**
 * Write `h` as an i386 control header at user address `at`.
 * The caller bounds its length to 32 bits and holds the big lock; the copy
 * may sleep. Returns 0 or -EFAULT.
 */
int ia32_cmsg_put(uintptr_t at, const struct kernel_cmsghdr *h)
{
    struct ia32_cmsghdr c;

    c.cmsg_len = (uint32_t)h->cmsg_len;
    c.cmsg_level = h->cmsg_level;
    c.cmsg_type = h->cmsg_type;
    return copy_to_user((void __user *)at, &c, sizeof(c)) != 0 ? -EFAULT : 0;
}

/**
 * socketcall (102): the sub-call and the address of its arguments, an array
 * of 32-bit words, as many as the sub-call takes (`socketcall_nargs`), each
 * zero-extended as Linux's compat_sys_socketcall reads them; the sub-call
 * then runs as the direct call would, its recvmmsg with an i386 timespec.
 * Returns the sub-call's result, -EINVAL for one it does not have, or
 * -EFAULT.
 */
long compat_sys_socketcall(isr_params_t *params)
{
    int call = (int)SC_ARG(params, 0);
    uintptr_t argp = (uint32_t)SC_ARG(params, 1);
    int nargs = socketcall_nargs(call);
    uint32_t w[6];
    unsigned long args[6];
    int i;

    if (nargs == 0) {
        return -EINVAL;
    }
    if (copy_from_user(w, (const void __user *)argp, (uint32_t)nargs * sizeof(w[0])) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < 6; i++) {
        args[i] = i < nargs ? (unsigned long)w[i] : 0ul;
    }
    return socket_call(call, args);
}

#endif /* RACH2_IA32 */
