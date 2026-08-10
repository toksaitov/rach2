/**
 * The host stand-in for musl's `<shadow.h>`.
 *
 * See `crypt.h` beside this file for why the compat headers exist. The fields
 * are musl's, in musl's order, because the library reads `sp_pwdp` through this
 * declaration when it is compiled natively; the reader path itself is proved in
 * the guest, against a real `/etc/shadow`.
 */
#ifndef _HOSTCOMPAT_SHADOW_H_
#define _HOSTCOMPAT_SHADOW_H_

struct spwd {
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};

struct spwd *getspnam(const char *name);

#endif /* _HOSTCOMPAT_SHADOW_H_ */
