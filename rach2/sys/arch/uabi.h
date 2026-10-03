#ifndef _UABI_H_
#define _UABI_H_

/*
 * The user structures whose layout differs by architecture past what a
 * portable declaration gives: each architecture's uabi.h has struct
 * user_stat, struct user_statfs and the System V IPC records of IPC_STAT,
 * with the macros STAT_SET_INO and IPC_TIME_SET, and ARCH_WANT_OLD_STAT
 * where the architecture has i386's old stat calls. The structures every
 * architecture lays out alike from their C types are declared here and
 * where they are used.
 */

#include "../lib/kint.h"

/* Linux's struct ipc64_perm. Its mode is 16 bits on i386 and 32 on x86-64;
 * the 16 bits here are its low half, little-endian, the rest a pad. */
struct ipc64_perm {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint16_t mode, pad1;
    uint16_t seq, pad2;
    unsigned long unused1, unused2;
};

#if defined(RACH2_HOSTTEST)
#include "i686/uabi.h"
#elif defined(__x86_64__)
#include "x86_64/uabi.h"
#elif defined(__i386__)
#include "i686/uabi.h"
#elif defined(__aarch64__)
#include "arm64/uabi.h"
#elif defined(__arm__)
#include "arm/uabi.h"
#else
#error "uabi.h: no architecture directory for this compiler's target"
#endif

#endif /* _UABI_H_ */
