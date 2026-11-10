/*
 * ia32_misc.c: the rest of the i386 calls whose arguments differ: the 16-bit
 * ids, nice, limits, times and sysinfo, the old uname forms, the round-robin
 * slice, arch_prctl and futex with 32-bit times, for the compat table
 * (ia32_systab.c).
 */

#include "ia32.h"
#include "ia32_abi.h"

#include "../arch.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/futex.h"
#include "../../proc/process.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

#if RACH2_IA32

/*
 * Most calls here run the x86-64 handler (`ia32_native`) with the i386
 * arguments widened, and convert the structure it reads or writes in kernel
 * memory, as ia32_fs.c does. A handler that reads its structure in the
 * caller's layout itself, under `in_compat_syscall`, is run on the i386
 * frame as it is.
 */

/* A user pointer of an i386 call, zero-extended, and a conversion buffer's
 * address, as native arguments. */
#define UPTR(v) ((unsigned long)(uint32_t)(v))
#define KPTR(p) ((unsigned long)(uintptr_t)(p))

/* The calls that answer ENOSYS, since Rach2 has them on neither kernel
 * (ia32.h). */
IA32_STUB(compat_sys_setfsuid16)
IA32_STUB(compat_sys_setfsgid16)
IA32_STUB(compat_sys_kexec_load)
IA32_STUB(compat_sys_keyctl)

/* An i386 argument as a native signed int, sign-extended. */
#define SARG(v) ((unsigned long)(long)(int32_t)(v))

/* The 32-bit id of a 16-bit id argument, as Linux's low2highuid: the
 * argument's low 16 bits, with the 16-bit -1, which asks for no change, as
 * the 32-bit -1. */
#define ID32(v) ((uint16_t)(v) == 0xFFFFu ? 0xFFFFFFFFul : (unsigned long)(uint16_t)(v))

/* The nice values and the step `nice` clamps its increment to. */
#define NICE_LOWEST  (-20)
#define NICE_HIGHEST 19
#define NICE_WIDTH   40

/** Run the native id call `fn` with the 16-bit ids of the i386 arguments
 * `first` to `first` + `n` - 1 widened (`ID32`) and the arguments before
 * `first` as pointers or descriptors, zero-extended. */
static long id16_call(isr_params_t *params, syscall_fn_t fn, unsigned first, unsigned n)
{
    unsigned long a[3];
    unsigned i;

    a[0] = UPTR(SC_ARG(params, 0));
    a[1] = UPTR(SC_ARG(params, 1));
    a[2] = UPTR(SC_ARG(params, 2));
    for (i = first; i < first + n; i++) {
        a[i] = ID32(a[i]);
    }
    return ia32_native(params, fn, NULL, 0, a[0], a[1], a[2], 0, 0, 0);
}

/** chown (182) and the 16-bit lchown (16) and fchown (95): the native call
 * with the owner and group widened, -1 leaving either as it is. */
long compat_sys_chown16(isr_params_t *params)
{
    return id16_call(params, sys_chown, 1, 2);
}

long compat_sys_lchown16(isr_params_t *params)
{
    return id16_call(params, sys_lchown, 1, 2);
}

long compat_sys_fchown16(isr_params_t *params)
{
    return id16_call(params, sys_fchown, 1, 2);
}

/** setuid (23), setgid (46), setreuid (70), setregid (71), setresuid (164)
 * and setresgid (170) with 16-bit ids: the native call with each widened. */
long compat_sys_setuid16(isr_params_t *params)
{
    return id16_call(params, sys_setuid, 0, 1);
}

long compat_sys_setgid16(isr_params_t *params)
{
    return id16_call(params, sys_setgid, 0, 1);
}

long compat_sys_setreuid16(isr_params_t *params)
{
    return id16_call(params, sys_setreuid, 0, 2);
}

long compat_sys_setregid16(isr_params_t *params)
{
    return id16_call(params, sys_setregid, 0, 2);
}

long compat_sys_setresuid16(isr_params_t *params)
{
    return id16_call(params, sys_setresuid, 0, 3);
}

long compat_sys_setresgid16(isr_params_t *params)
{
    return id16_call(params, sys_setresgid, 0, 3);
}

/** getuid (24), getgid (47), geteuid (49) and getegid (50): the native
 * id in 16 bits, 65534 for one past 65535 (`IA32_ID16`). */
long compat_sys_getuid16(isr_params_t *params)
{
    return IA32_ID16(sys_getuid(params));
}

long compat_sys_getgid16(isr_params_t *params)
{
    return IA32_ID16(sys_getgid(params));
}

long compat_sys_geteuid16(isr_params_t *params)
{
    return IA32_ID16(sys_geteuid(params));
}

long compat_sys_getegid16(isr_params_t *params)
{
    return IA32_ID16(sys_getegid(params));
}

/** Run the native getresuid or getresgid `fn` into a buffer and write its
 * three ids to the i386 caller's three 16-bit words; 0 or -EFAULT. */
static long getres16(isr_params_t *params, syscall_fn_t fn)
{
    uint32_t ids[3];
    unsigned long u[3];
    uint16_t id;
    long rc;
    unsigned i;

    rc = ia32_native(params, fn, ids, sizeof(ids), KPTR(&ids[0]), KPTR(&ids[1]), KPTR(&ids[2]), 0,
                     0, 0);
    if (rc != 0) {
        return rc;
    }
    u[0] = UPTR(SC_ARG(params, 0));
    u[1] = UPTR(SC_ARG(params, 1));
    u[2] = UPTR(SC_ARG(params, 2));
    for (i = 0; i < 3u; i++) {
        id = IA32_ID16(ids[i]);
        if (copy_to_user((void __user *)u[i], &id, sizeof(id)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/** getresuid (165) and getresgid (171) with 16-bit ids. */
long compat_sys_getresuid16(isr_params_t *params)
{
    return getres16(params, sys_getresuid);
}

long compat_sys_getresgid16(isr_params_t *params)
{
    return getres16(params, sys_getresgid);
}

/**
 * getgroups (80) with 16-bit ids: the native call into a buffer of
 * NGROUPS_MAX ids, then each id in 16 bits. A size of 0 returns the count,
 * one smaller than the count is -EINVAL, as the native call has it; a list
 * that cannot be written is -EFAULT.
 */
long compat_sys_getgroups16(isr_params_t *params)
{
    int32_t size = (int32_t)SC_ARG(params, 0);
    uint32_t groups[NGROUPS_MAX];
    uint16_t g;
    long rc;
    long i;

    if (size < 0) {
        return -EINVAL;
    }
    if (size > NGROUPS_MAX) {
        size = NGROUPS_MAX;
    }
    rc = ia32_native(params, sys_getgroups, groups, sizeof(groups), (unsigned long)size,
                     KPTR(groups), 0, 0, 0, 0);
    if (rc <= 0 || size == 0) {
        return rc;
    }
    for (i = 0; i < rc; i++) {
        g = IA32_ID16(groups[i]);
        if (copy_to_user((void __user *)(UPTR(SC_ARG(params, 1)) + (unsigned long)i * 2u), &g,
                         sizeof(g)) != 0) {
            return -EFAULT;
        }
    }
    return rc;
}

/**
 * setgroups (81) with 16-bit ids: the list read and widened into a buffer,
 * then the native call. A size past NGROUPS_MAX, or a list that cannot be
 * read, goes to the native call as it is, which answers its errors in their
 * order: -EINVAL, -EPERM, then -EFAULT.
 */
long compat_sys_setgroups16(isr_params_t *params)
{
    uint32_t size = (uint32_t)SC_ARG(params, 0);
    unsigned long list = UPTR(SC_ARG(params, 1));
    uint32_t groups[NGROUPS_MAX];
    uint16_t g;
    uint32_t i;

    if (size > NGROUPS_MAX) {
        return ia32_native(params, sys_setgroups, NULL, 0, size, list, 0, 0, 0, 0);
    }
    for (i = 0; i < size; i++) {
        if (copy_from_user(&g, (const void __user *)(list + i * 2u), sizeof(g)) != 0) {
            return ia32_native(params, sys_setgroups, NULL, 0, size, 0, 0, 0, 0, 0);
        }
        groups[i] = (uint32_t)ID32(g);
    }
    return ia32_native(params, sys_setgroups, groups, sizeof(groups), size, KPTR(groups), 0, 0, 0,
                       0);
}

/** umount (22): umount2 with no flags, as Linux's oldumount. */
long compat_sys_oldumount(isr_params_t *params)
{
    return ia32_native(params, sys_umount2, NULL, 0, UPTR(SC_ARG(params, 0)), 0, 0, 0, 0, 0);
}

/**
 * nice (34): add the increment, clamped to 40 either way, to the caller's
 * nice value, which stays within -20 to 19, as Linux's sys_nice. A negative
 * increment needs privilege, -EPERM without it, since Rach2 keeps no
 * RLIMIT_NICE and Linux's default of 0 lets no one else lower the value.
 * The value is set by setpriority.
 */
long compat_sys_nice(isr_params_t *params)
{
    int32_t inc = (int32_t)SC_ARG(params, 0);
    int32_t nice;

    if (inc < -NICE_WIDTH) {
        inc = -NICE_WIDTH;
    }
    if (inc > NICE_WIDTH) {
        inc = NICE_WIDTH;
    }
    nice = current_proc->nice + inc;
    if (nice < NICE_LOWEST) {
        nice = NICE_LOWEST;
    }
    if (nice > NICE_HIGHEST) {
        nice = NICE_HIGHEST;
    }
    if (inc < 0 && !cred_privileged()) {
        return -EPERM;
    }
    return ia32_native(params, sys_setpriority, NULL, 0, 0, 0, SARG(nice), 0, 0, 0);
}

/**
 * times (43): the native times into a buffer, then the four counts in 32
 * bits, as Linux's compat times; the return value is the native one, of
 * which the i386 caller sees the low 32 bits. -EFAULT for a structure that
 * cannot be written.
 */
long compat_sys_times(isr_params_t *params)
{
    unsigned long u = UPTR(SC_ARG(params, 0));
    long t[4];
    struct ia32_tms t32;
    long rc;

    if (u == 0) {
        return ia32_native(params, sys_times, NULL, 0, 0, 0, 0, 0, 0, 0);
    }
    rc = ia32_native(params, sys_times, t, sizeof(t), KPTR(t), 0, 0, 0, 0, 0);
    t32.tms_utime = (int32_t)t[0];
    t32.tms_stime = (int32_t)t[1];
    t32.tms_cutime = (int32_t)t[2];
    t32.tms_cstime = (int32_t)t[3];
    if (copy_to_user((void __user *)u, &t32, sizeof(t32)) != 0) {
        return -EFAULT;
    }
    return rc;
}

/** Run the native uname into `uts`, its six fields of IA32_UTS_LEN bytes;
 * 0, or its error. */
static long uname_native(isr_params_t *params, char uts[6][IA32_UTS_LEN])
{
    return ia32_native(params, sys_uname, uts, 6u * IA32_UTS_LEN, KPTR(uts), 0, 0, 0, 0, 0);
}

/**
 * olduname (109): the first five fields of uname, without the domain name,
 * in struct old_utsname, as Linux's sys_uname writes them; -EFAULT.
 */
long compat_sys_uname(isr_params_t *params)
{
    char uts[6][IA32_UTS_LEN];
    long rc = uname_native(params, uts);

    if (rc != 0) {
        return rc;
    }
    if (copy_to_user((void __user *)UPTR(SC_ARG(params, 0)), uts, IA32_UTS_FIELDS * IA32_UTS_LEN) !=
        0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * oldolduname (59): the first five fields of uname, each cut to 8 bytes and
 * a null, in struct oldold_utsname, as Linux's sys_olduname writes them;
 * -EFAULT.
 */
long compat_sys_olduname(isr_params_t *params)
{
    char uts[6][IA32_UTS_LEN];
    char old[IA32_UTS_FIELDS][IA32_OLD_UTS_LEN];
    long rc = uname_native(params, uts);
    unsigned i;

    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < IA32_UTS_FIELDS; i++) {
        k_memcpy(old[i], uts[i], IA32_OLD_UTS_LEN - 1u);
        old[i][IA32_OLD_UTS_LEN - 1u] = '\0';
    }
    if (copy_to_user((void __user *)UPTR(SC_ARG(params, 0)), old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* Linux's compat unlimited, which a limit past 32 bits reads as. */
#define IA32_RLIM_INFINITY 0xFFFFFFFFul

/**
 * ugetrlimit (191): the native getrlimit into a buffer, then the limits in
 * 32 bits, one past 0xFFFFFFFF, unlimited among them, as 0xFFFFFFFF, as
 * Linux's compat getrlimit; -EINVAL for an unknown resource, or -EFAULT.
 * The old getrlimit (76) is the shared `sys_old_getrlimit`.
 */
long compat_sys_getrlimit(isr_params_t *params)
{
    unsigned long most = IA32_RLIM_INFINITY;
    unsigned long rl[2];
    struct ia32_rlimit r32;
    long rc;

    rc = ia32_native(params, sys_getrlimit, rl, sizeof(rl), (uint32_t)SC_ARG(params, 0), KPTR(rl),
                     0, 0, 0, 0);
    if (rc != 0) {
        return rc;
    }
    r32.rlim_cur = (uint32_t)(rl[0] > most ? most : rl[0]);
    r32.rlim_max = (uint32_t)(rl[1] > most ? most : rl[1]);
    if (copy_to_user((void __user *)UPTR(SC_ARG(params, 1)), &r32, sizeof(r32)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** setrlimit (75): the i386 limits read, 0xFFFFFFFF as unlimited, then the
 * native setrlimit; -EFAULT, or its error. */
long compat_sys_setrlimit(isr_params_t *params)
{
    struct ia32_rlimit r32;
    unsigned long rl[2];

    if (copy_from_user(&r32, (const void __user *)UPTR(SC_ARG(params, 1)), sizeof(r32)) != 0) {
        return -EFAULT;
    }
    rl[0] = r32.rlim_cur == IA32_RLIM_INFINITY ? RLIM_INFINITY : (unsigned long)r32.rlim_cur;
    rl[1] = r32.rlim_max == IA32_RLIM_INFINITY ? RLIM_INFINITY : (unsigned long)r32.rlim_max;
    return ia32_native(params, sys_setrlimit, rl, sizeof(rl), (uint32_t)SC_ARG(params, 0), KPTR(rl),
                       0, 0, 0, 0);
}

/* The x86-64 struct sysinfo the native call writes, 112 bytes. */
struct native_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    uint16_t procs;
    uint16_t pad;
    unsigned long totalhigh, freehigh;
    uint32_t mem_unit;
};

/**
 * sysinfo (116): the native sysinfo into a buffer, then in 32 bits, as
 * Linux's compat sysinfo: when the RAM or the swap does not fit 32 bits, the
 * unit doubles, up to a page, and each size halves with it. -EFAULT.
 */
long compat_sys_sysinfo(isr_params_t *params)
{
    struct native_sysinfo s;
    struct ia32_sysinfo s32;
    unsigned shift = 0;
    long rc;

    rc = ia32_native(params, sys_sysinfo, &s, sizeof(s), KPTR(&s), 0, 0, 0, 0, 0);
    if (rc != 0) {
        return rc;
    }
    if ((s.totalram >> 32) != 0 || (s.totalswap >> 32) != 0) {
        while (s.mem_unit < PAGE_SIZE) {
            s.mem_unit <<= 1;
            shift++;
        }
    }
    k_memset(&s32, 0, sizeof(s32));
    s32.uptime = (int32_t)s.uptime;
    s32.loads[0] = (uint32_t)s.loads[0];
    s32.loads[1] = (uint32_t)s.loads[1];
    s32.loads[2] = (uint32_t)s.loads[2];
    s32.totalram = (uint32_t)(s.totalram >> shift);
    s32.freeram = (uint32_t)(s.freeram >> shift);
    s32.sharedram = (uint32_t)(s.sharedram >> shift);
    s32.bufferram = (uint32_t)(s.bufferram >> shift);
    s32.totalswap = (uint32_t)(s.totalswap >> shift);
    s32.freeswap = (uint32_t)(s.freeswap >> shift);
    s32.procs = s.procs;
    s32.totalhigh = (uint32_t)(s.totalhigh >> shift);
    s32.freehigh = (uint32_t)(s.freehigh >> shift);
    s32.mem_unit = s.mem_unit;
    if (copy_to_user((void __user *)UPTR(SC_ARG(params, 0)), &s32, sizeof(s32)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * sched_rr_get_interval (161): the native call into a buffer, then the
 * slice as an i386 timespec; -EINVAL, -ESRCH or -EFAULT.
 */
long compat_sys_sched_rr_get_interval_time32(isr_params_t *params)
{
    long ts[2];
    struct ia32_timespec t32;
    long rc;

    rc = ia32_native(params, sys_sched_rr_get_interval, ts, sizeof(ts), SARG(SC_ARG(params, 0)),
                     KPTR(ts), 0, 0, 0, 0);
    if (rc != 0) {
        return rc;
    }
    t32.tv_sec = (int32_t)ts[0];
    t32.tv_nsec = (int32_t)ts[1];
    if (copy_to_user((void __user *)UPTR(SC_ARG(params, 1)), &t32, sizeof(t32)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * arch_prctl (384): Linux's i386 call takes only the commands it shares
 * with x86-64's, the CPUID and XCOMP ones, and refuses the segment base
 * commands with -EINVAL. Rach2 has none of the shared commands, which the
 * x86-64 call answers with -EINVAL too, so every command is -EINVAL.
 */
long compat_sys_arch_prctl(isr_params_t *params)
{
    (void)params;
    return -EINVAL;
}

/* The command of a futex operation, its low seven bits. */
#define FUTEX_CMD_MASK 0x7Fu

/** Whether the futex command `cmd` takes a timeout, as Linux's
 * futex_cmd_has_timeout: FUTEX_WAIT, FUTEX_LOCK_PI, FUTEX_WAIT_BITSET,
 * FUTEX_WAIT_REQUEUE_PI and FUTEX_LOCK_PI2. */
static int futex_has_timeout(uint32_t cmd)
{
    return cmd == 0 || cmd == 6 || cmd == 9 || cmd == 11 || cmd == 13;
}

/**
 * futex (240): the x86-64 futex with the i386 timespec of an operation that
 * takes a timeout read into a native one first, as Linux's futex_time32
 * reads it; another operation's fourth argument is a value and passes as it
 * is. -EFAULT for a timeout that cannot be read.
 */
long compat_sys_futex_time32(isr_params_t *params)
{
    uint32_t op = (uint32_t)SC_ARG(params, 1);
    unsigned long a3 = UPTR(SC_ARG(params, 3));
    long ts[2];
    void *win = NULL;

    if (a3 != 0 && futex_has_timeout(op & FUTEX_CMD_MASK)) {
        if (ia32_get_timespec(a3, ts) != 0) {
            return -EFAULT;
        }
        win = ts;
        a3 = KPTR(ts);
    }
    return ia32_native(params, sys_futex, win, sizeof(ts), UPTR(SC_ARG(params, 0)), op,
                       (uint32_t)SC_ARG(params, 2), a3, UPTR(SC_ARG(params, 4)),
                       (uint32_t)SC_ARG(params, 5));
}

#endif /* RACH2_IA32 */
