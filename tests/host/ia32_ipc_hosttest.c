/**
 * ia32_ipc_hosttest.c: the i386 System V IPC and message queue records of
 * rach2/sys/ipc/ipc32.h, the limit, usage, sysinfo and tms records of
 * rach2/sys/arch/x86_64/ia32_abi.h and the 32-bit robust list head of
 * rach2/sys/proc/futex.h, which the x86-64 kernel reads and writes for an
 * i386 caller, against Linux 6.12's: each structure's size and the offset of
 * each member Linux's compat code fills.
 *
 * The rows hold Linux's numbers for i386. They were read from Linux 6.12's
 * include/uapi/linux/ipc.h, sem.h, shm.h, msg.h, mqueue.h, resource.h,
 * sysinfo.h, times.h and futex.h, arch/x86/include/uapi/asm/sembuf.h and
 * include/uapi/asm-generic/ipcbuf.h, shmbuf.h and msgbuf.h, whose i386
 * forms are those of the compat types in include/linux/compat.h and
 * ipc/util.h, laid out by i686-elf-gcc with offsetof and sizeof. Every
 * member of these records is a word of 32 bits or fewer, so the host lays
 * each out as i386 does, whatever its own ABI.
 */
#include "arch/x86_64/ia32_abi.h"
#include "ipc/ipc32.h"
#include "proc/futex.h"

#include <stddef.h>

extern int printf(const char *format, ...);

/* A structure's size or a member's offset, and Linux's. */
struct row {
    const char *what;
    unsigned long found;
    unsigned long linux;
};

#define SIZE(t, n)   {#t, sizeof(struct t), n}
#define OFF(t, f, n) {#t "." #f, offsetof(struct t, f), n}

static const struct row rows[] = {
    /* ipc_perm, of the records without IPC_64 */
    SIZE(ipc32_perm, 16),
    OFF(ipc32_perm, uid, 4),
    OFF(ipc32_perm, gid, 6),
    OFF(ipc32_perm, cuid, 8),
    OFF(ipc32_perm, cgid, 10),
    OFF(ipc32_perm, mode, 12),
    OFF(ipc32_perm, seq, 14),
    /* semid_ds */
    SIZE(ipc32_semid_ds, 44),
    OFF(ipc32_semid_ds, otime, 16),
    OFF(ipc32_semid_ds, ctime, 20),
    OFF(ipc32_semid_ds, base, 24),
    OFF(ipc32_semid_ds, undo, 36),
    OFF(ipc32_semid_ds, nsems, 40),
    /* shmid_ds */
    SIZE(ipc32_shmid_ds, 48),
    OFF(ipc32_shmid_ds, segsz, 16),
    OFF(ipc32_shmid_ds, atime, 20),
    OFF(ipc32_shmid_ds, dtime, 24),
    OFF(ipc32_shmid_ds, ctime, 28),
    OFF(ipc32_shmid_ds, cpid, 32),
    OFF(ipc32_shmid_ds, lpid, 34),
    OFF(ipc32_shmid_ds, nattch, 36),
    OFF(ipc32_shmid_ds, unused2, 40),
    /* msqid_ds */
    SIZE(ipc32_msqid_ds, 56),
    OFF(ipc32_msqid_ds, first, 16),
    OFF(ipc32_msqid_ds, stime, 24),
    OFF(ipc32_msqid_ds, rtime, 28),
    OFF(ipc32_msqid_ds, ctime, 32),
    OFF(ipc32_msqid_ds, lcbytes, 36),
    OFF(ipc32_msqid_ds, lqbytes, 40),
    OFF(ipc32_msqid_ds, cbytes, 44),
    OFF(ipc32_msqid_ds, qnum, 46),
    OFF(ipc32_msqid_ds, qbytes, 48),
    OFF(ipc32_msqid_ds, lspid, 50),
    OFF(ipc32_msqid_ds, lrpid, 52),
    /* shminfo, of IPC_INFO without IPC_64 */
    SIZE(ipc32_shminfo, 20),
    OFF(ipc32_shminfo, shmall, 16),
    /* ipc64_perm */
    SIZE(ipc32_perm64, 36),
    OFF(ipc32_perm64, uid, 4),
    OFF(ipc32_perm64, cgid, 16),
    OFF(ipc32_perm64, mode, 20),
    OFF(ipc32_perm64, seq, 24),
    OFF(ipc32_perm64, unused1, 28),
    /* semid64_ds */
    SIZE(ipc32_semid64_ds, 64),
    OFF(ipc32_semid64_ds, otime, 36),
    OFF(ipc32_semid64_ds, otime_high, 40),
    OFF(ipc32_semid64_ds, ctime, 44),
    OFF(ipc32_semid64_ds, ctime_high, 48),
    OFF(ipc32_semid64_ds, nsems, 52),
    /* shmid64_ds */
    SIZE(ipc32_shmid64_ds, 84),
    OFF(ipc32_shmid64_ds, segsz, 36),
    OFF(ipc32_shmid64_ds, atime, 40),
    OFF(ipc32_shmid64_ds, atime_high, 44),
    OFF(ipc32_shmid64_ds, dtime, 48),
    OFF(ipc32_shmid64_ds, ctime, 56),
    OFF(ipc32_shmid64_ds, ctime_high, 60),
    OFF(ipc32_shmid64_ds, cpid, 64),
    OFF(ipc32_shmid64_ds, lpid, 68),
    OFF(ipc32_shmid64_ds, nattch, 72),
    /* msqid64_ds */
    SIZE(ipc32_msqid64_ds, 88),
    OFF(ipc32_msqid64_ds, stime, 36),
    OFF(ipc32_msqid64_ds, rtime, 44),
    OFF(ipc32_msqid64_ds, ctime, 52),
    OFF(ipc32_msqid64_ds, ctime_high, 56),
    OFF(ipc32_msqid64_ds, cbytes, 60),
    OFF(ipc32_msqid64_ds, qnum, 64),
    OFF(ipc32_msqid64_ds, qbytes, 68),
    OFF(ipc32_msqid64_ds, lspid, 72),
    OFF(ipc32_msqid64_ds, lrpid, 76),
    /* shminfo64 and shm_info */
    SIZE(ipc32_shminfo64, 36),
    OFF(ipc32_shminfo64, shmall, 16),
    SIZE(ipc32_shm_info, 24),
    OFF(ipc32_shm_info, shm_tot, 4),
    OFF(ipc32_shm_info, swap_successes, 20),
    /* msgbuf's text after the type, and ipc(2)'s msgrcv pair */
    SIZE(ipc32_msgbuf, 4),
    SIZE(ipc32_kludge, 8),
    OFF(ipc32_kludge, msgtyp, 4),
    /* mq_attr */
    SIZE(ipc32_mq_attr, 32),
    OFF(ipc32_mq_attr, maxmsg, 4),
    OFF(ipc32_mq_attr, msgsize, 8),
    OFF(ipc32_mq_attr, curmsgs, 12),
    OFF(ipc32_mq_attr, reserved, 16),
    /* rlimit */
    SIZE(ia32_rlimit, 8),
    OFF(ia32_rlimit, rlim_max, 4),
    /* rusage */
    SIZE(ia32_rusage, 72),
    OFF(ia32_rusage, ru_stime, 8),
    OFF(ia32_rusage, ru_maxrss, 16),
    OFF(ia32_rusage, ru_minflt, 32),
    OFF(ia32_rusage, ru_majflt, 36),
    OFF(ia32_rusage, ru_nivcsw, 68),
    /* sysinfo */
    SIZE(ia32_sysinfo, 64),
    OFF(ia32_sysinfo, loads, 4),
    OFF(ia32_sysinfo, totalram, 16),
    OFF(ia32_sysinfo, bufferram, 28),
    OFF(ia32_sysinfo, freeswap, 36),
    OFF(ia32_sysinfo, procs, 40),
    OFF(ia32_sysinfo, totalhigh, 44),
    OFF(ia32_sysinfo, freehigh, 48),
    OFF(ia32_sysinfo, mem_unit, 52),
    /* tms */
    SIZE(ia32_tms, 16),
    OFF(ia32_tms, tms_stime, 4),
    OFF(ia32_tms, tms_cstime, 12),
    /* robust_list_head */
    SIZE(robust_head32, 12),
    OFF(robust_head32, futex_offset, 4),
    OFF(robust_head32, op_pending, 8),
    /* old_utsname's five fields and oldold_utsname's */
    {"ia32 old_utsname", IA32_UTS_FIELDS *IA32_UTS_LEN, 325},
    {"ia32 oldold_utsname", IA32_UTS_FIELDS *IA32_OLD_UTS_LEN, 45},
};

int main(void)
{
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].found != rows[i].linux) {
            printf("ia32_ipc: %s: %lu, Linux %lu\n", rows[i].what, rows[i].found, rows[i].linux);
            bad++;
        }
    }
    if (bad) {
        printf("ia32_ipc: result: %d faults\n", bad);
        return 1;
    }
    printf("ia32_ipc: result: %lu layout rows match Linux 6.12\n",
           (unsigned long)(sizeof(rows) / sizeof(rows[0])));
    return 0;
}
