/* process_vm_readv(2) and process_vm_writev(2): copies between the memory
 * of two processes, under ptrace's permission rules. */
#ifndef _PROCESS_VM_H_
#define _PROCESS_VM_H_

#include "../irq.h"

/** process_vm_readv(pid, local_iov, liovcnt, remote_iov, riovcnt,
 * flags); the bytes copied, or -errno. */
long sys_process_vm_readv(isr_params_t *params);

/** process_vm_writev(pid, local_iov, liovcnt, remote_iov, riovcnt,
 * flags); the bytes copied, or -errno. */
long sys_process_vm_writev(isr_params_t *params);

/** process_madvise(pidfd, iovec, vlen, advice, flags): advice about the
 * memory of the process a pidfd names; the bytes advised or -errno. */
long sys_process_madvise(isr_params_t *params);

/** process_mrelease(pidfd, flags): free the memory of the dying process a
 * pidfd names; 0 or -errno. */
long sys_process_mrelease(isr_params_t *params);

#endif /* _PROCESS_VM_H_ */
