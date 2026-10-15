/**
 * ownprobe: file ownership, the ids a new file is given, the umask, and the
 * permission rules of chmod, chown and kill.
 *
 * A new file belongs to its creator's effective ids with the file-creation
 * mask applied; chmod is the owner's or root's; a uid change by chown is
 * root's alone, while an owner may chgrp only to a group it belongs to; the
 * fd variants, fchmod and fchown, obey the same rules; and kill answers
 * EPERM, not ESRCH, for a live process the sender does not own, except
 * SIGCONT within the sender's session, and lets a sender reach a process
 * whose saved uid is its own. The probe removes what it creates and restores
 * the umask, so that the boot it shares with the probes after it is clean.
 *
 * Prints "OWNPROBE OK" and exits 0 on success; prints the failing step and
 * exits 1 otherwise. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int u32;

#define SIGCONT 18
#define SIGTERM 15

#define STDOUT 1
#define EPERM  1
#define KEEP   0xFFFFFFFFu

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` to standard output. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** Print "OWNPROBE FAIL: `step`" and exit 1. */
static void fail(const char *step)
{
    puts_("OWNPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/** The 32-bit field at `offset`, one of probe.h's KSTAT_ offsets, of the
 * stat of `path`; a stat that fails fails the probe. */
static u32 stat_field(const char *path, int offset)
{
    static char st[144];

    if (syscall3(SYS_STAT, (long)path, (long)st, 0) != 0) {
        fail("stat");
    }
    return *(u32 *)(st + offset);
}

/**
 * The rows that change the process's ids, run in a child so that the parent
 * stays root: creation under umask 027 by root and by an effective uid of
 * 1000, chown as root, then as uid 1000 with the supplementary group 3000
 * chmod, fchmod, chown and chgrp, and kill. Exits 0, or fails the probe.
 *
 * kill of the root parent is EPERM and of the child itself succeeds, which
 * shows that the uid drop took. SIGCONT reaches the parent within the
 * session, as a shell resumes a job it started through su, whose real uid
 * is 0 and the shell's is not; after setsid it is EPERM too. setsid only
 * creates a session, so no process can move into one to gain that reach.
 */
static void child_main(void)
{
    u32 groups[1];
    int fd;

    /* umask 027 turns 0666 into 0640 and 0777 into 0750, the creator's. */
    syscall3(SYS_UMASK, 027, 0, 0);
    fd = syscall3(SYS_CREAT, (long)"/own.txt", 0666, 0);
    if (fd < 0) {
        fail("creat");
    }
    if ((stat_field("/own.txt", KSTAT_MODE) & 0777) != 0640) {
        fail("umask on creat");
    }
    if (stat_field("/own.txt", KSTAT_UID) != 0 || stat_field("/own.txt", KSTAT_GID) != 0) {
        fail("creation owner not root");
    }
    if (syscall3(SYS_MKDIR, (long)"/own.d", 0777, 0) != 0 ||
        (stat_field("/own.d", KSTAT_MODE) & 0777) != 0750) {
        fail("umask on mkdir");
    }

    /* chown: a uid change is root's; the stored ids come back from stat. */
    if (syscall3(SYS_CHOWN, (long)"/own.txt", 1000, 1000) != 0 ||
        stat_field("/own.txt", KSTAT_UID) != 1000 || stat_field("/own.txt", KSTAT_GID) != 1000) {
        fail("chown as root");
    }
    if (syscall3(SYS_FCHOWN, fd, 1000, 2000) != 0 || stat_field("/own.txt", KSTAT_GID) != 2000) {
        fail("fchown as root");
    }
    if (syscall3(SYS_CHOWN, (long)"/own.d", 1000, 1000) != 0) {
        fail("chown dir as root");
    }

    /* Real uid 0, effective 1000: the file is 1000's; chmod is not masked. */
    if (syscall3(SYS_SETREUID, (int)KEEP, 1000, 0) != 0) {
        fail("excursion out");
    }
    if (syscall3(SYS_CREAT, (long)"/own.d/e.txt", 0666, 0) < 0 ||
        stat_field("/own.d/e.txt", KSTAT_UID) != 1000 ||
        (stat_field("/own.d/e.txt", KSTAT_MODE) & 0777) != 0640) {
        fail("creation stamps euid under umask");
    }
    if (syscall3(SYS_CHMOD, (long)"/own.d/e.txt", 0666, 0) != 0 ||
        (stat_field("/own.d/e.txt", KSTAT_MODE) & 0777) != 0666) {
        fail("chmod is not masked");
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != 0) {
        fail("excursion back");
    }

    /* Drop to 1000 with supplementary group 3000 for the rules below. */
    groups[0] = 3000;
    if (syscall3(SYS_SETGROUPS, 1, (long)groups, 0) != 0 ||
        syscall3(SYS_SETRESGID, 1000, 1000, 1000) != 0 ||
        syscall3(SYS_SETRESUID, 1000, 1000, 1000) != 0) {
        fail("drop to 1000");
    }

    /* chmod: the owner may; fchmod agrees; a uid change stays root's. */
    if (syscall3(SYS_CHMOD, (long)"/own.txt", 0600, 0) != 0 ||
        (stat_field("/own.txt", KSTAT_MODE) & 0777) != 0600) {
        fail("chmod as owner");
    }
    if (syscall3(SYS_FCHMOD, fd, 0640, 0) != 0 ||
        (stat_field("/own.txt", KSTAT_MODE) & 0777) != 0640) {
        fail("fchmod as owner");
    }
    if (syscall3(SYS_CHOWN, (long)"/own.txt", 0, KEEP) != -EPERM) {
        fail("chown uid as non-root not EPERM");
    }
    /* chgrp: the owner, to an own group only. */
    if (syscall3(SYS_CHOWN, (long)"/own.txt", KEEP, 3000) != 0 ||
        stat_field("/own.txt", KSTAT_GID) != 3000) {
        fail("owner chgrp to own group");
    }
    if (syscall3(SYS_CHOWN, (long)"/own.txt", KEEP, 4000) != -EPERM) {
        fail("owner chgrp to foreign group not EPERM");
    }
    /* A non-owner may not chmod: /hello.txt is root's, the caller 1000. */
    if (syscall3(SYS_CHMOD, (long)"/hello.txt", 0777, 0) != -EPERM) {
        fail("chmod as non-owner not EPERM");
    }
    /* fchmod on a descriptor of /own.d, the caller's directory. */
    {
        int dfd = syscall3(SYS_OPEN, (long)"/own.d", 0, 0);
        if (dfd < 0) {
            fail("open own dir");
        }
        if (syscall3(SYS_FCHMOD, dfd, 0700, 0) != 0 ||
            (stat_field("/own.d", KSTAT_MODE) & 0777) != 0700) {
            fail("fchmod on dir fd");
        }
        syscall3(SYS_CLOSE, dfd, 0, 0);
    }

    /* kill: the parent is root's, so uid 1000 gets EPERM; itself, 0. */
    {
        int ppid = syscall3(SYS_GETPPID, 0, 0, 0);

        if (syscall3(SYS_KILL, ppid, 0, 0) != -EPERM) {
            fail("kill root parent not EPERM");
        }
        if (syscall3(SYS_KILL, syscall3(SYS_GETPID, 0, 0, 0), 0, 0) != 0) {
            fail("kill self probe");
        }
        if (syscall3(SYS_KILL, ppid, SIGTERM, 0) != -EPERM) {
            fail("SIGTERM to the root parent not EPERM");
        }

        /* SIGCONT passes within the session, as a shell resumes a su job. */
        if (syscall3(SYS_KILL, ppid, SIGCONT, 0) != 0) {
            fail("SIGCONT to the root parent in our session");
        }

        /* Out of the session, SIGCONT is EPERM too. */
        if (syscall3(SYS_SETSID, 0, 0, 0) < 0) {
            fail("setsid");
        }
        if (syscall3(SYS_KILL, ppid, SIGCONT, 0) != -EPERM) {
            fail("SIGCONT across sessions not EPERM");
        }
    }

    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_EXIT, 0, 0, 0);
}

/**
 * The probe's entry, root throughout: runs `child_main`, then the saved-uid
 * case, then removes what the probe created and restores umask 022.
 *
 * The saved-uid case is one nothing in the shipped images reaches, since su
 * and dropbear both drop all three ids: only a process that keeps an id in
 * its saved slot can be signalled through it. The parent keeps 1000 there
 * while it stays root, and the child, uid 1000, sends it signal 0, since a
 * real signal would reach the probe itself and could end the run.
 */
PROBE_START void _start(void)
{
    int pid, status;

    syscall3(SYS_UMASK, 022, 0, 0);
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child_main();
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("child walk");
    }

    /* The saved-uid case: 1000 in the parent's saved slot. */
    if (syscall3(SYS_SETRESUID, 0, 0, 1000) != 0) {
        fail("park 1000 in the saved slot");
    }
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork for the saved-uid case");
    }
    if (pid == 0) {
        int ppid = syscall3(SYS_GETPPID, 0, 0, 0);

        if (syscall3(SYS_SETRESUID, 1000, 1000, 1000) != 0) {
            syscall3(SYS_EXIT, 21, 0, 0);
        }
        if (syscall3(SYS_KILL, ppid, 0, 0) != 0) {
            syscall3(SYS_EXIT, 22, 0, 0);
        }
        syscall3(SYS_EXIT, 0, 0, 0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("the saved-uid child");
    }
    if (syscall3(SYS_SETRESUID, 0, 0, 0) != 0) {
        fail("restore the saved uid");
    }

    /* Remove what the probe created, and restore the mask. */
    if (syscall3(SYS_UNLINK, (long)"/own.txt", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/own.d/e.txt", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/own.d", 0, 0) != 0) {
        fail("cleanup");
    }
    syscall3(SYS_UMASK, 022, 0, 0);

    puts_("OWNPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
