/**
 * Verify file ownership: creation stamping, the umask, and the chmod/chown
 * and kill permission rules.
 *
 * A new file belongs to its creator's effective ids with the file-creation
 * mask applied; chmod is the owner's or root's; a uid change by chown is
 * root's alone while an owner may chgrp only to a group it belongs to; the fd
 * variants obey the same rules through fchmod/fchown; and kill answers EPERM,
 * not ESRCH, for a live process the sender does not own. All state the probe
 * creates is removed and the umask restored, so the shared boot stays clean
 * for the probes that follow.
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

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

static void fail(const char *step)
{
    puts_("OWNPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/* The stat64 fields this probe reads: mode at offset 16, uid at 24, gid at 28
 * (the i386 struct stat64 layout the kernel fills). */
static u32 stat_field(const char *path, int offset)
{
    static char st[144];

    if (syscall3(SYS_STAT, (long)path, (long)st, 0) != 0) {
        fail("stat");
    }
    return *(u32 *)(st + offset);
}

/* The identity-dependent walk runs in a child so the parent stays root. */
static void child_main(void)
{
    u32 groups[1];
    int fd;

    /* Creation applies the mask and stamps the creator: umask 027 turns 0666
     * into 0640 and 0777 into 0750, owned by the effective ids. */
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

    /* Creation stamps the EFFECTIVE id: with ruid 0 and euid 1000, the new
     * file belongs to 1000, still under the 027 mask. chmod then sets 0666
     * exactly: the mask applies at creation, never to chmod. */
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
    /* A non-owner may not chmod: /hello.txt is root's and we are 1000. */
    if (syscall3(SYS_CHMOD, (long)"/hello.txt", 0777, 0) != -EPERM) {
        fail("chmod as non-owner not EPERM");
    }
    /* The fd variants agree for a directory descriptor: /own.d is ours. */
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

    /* kill: the probe's parent is root; a 1000 sender gets EPERM, not
     * silence, and may still probe itself. These two are the paired
     * assertions that prove the uid drop above actually took, so the SIGCONT
     * case below cannot be read as the rule failing open. */
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

        /* SIGCONT passes within the session, which is what lets a shell resume
         * a job it started through su: that job's real uid is 0 and the shell's
         * is not. The probe shares its parent's session here. */
        if (syscall3(SYS_KILL, ppid, SIGCONT, 0) != 0) {
            fail("SIGCONT to the root parent in our session");
        }

        /* Leave the session and the exception goes with it, which is what
         * keeps it from being a way to reach anything at all: setsid only ever
         * creates a session, so nothing can move into one to gain reach. */
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

    /* The saved-uid arm, which nothing in the shipped images reaches: su and
     * dropbear both drop all three ids, so only a process that deliberately
     * parks an identity in its saved slot can be signalled through it. The
     * parent keeps 1000 there while staying root, and the child probes with
     * signal 0, not a real one, which would land on this probe's own
     * init and could end the run. */
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

    /* The parent is still root: remove the probe's droppings and restore the
     * mask, so the shared boot stays clean for the next probe. */
    if (syscall3(SYS_UNLINK, (long)"/own.txt", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/own.d/e.txt", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/own.d", 0, 0) != 0) {
        fail("cleanup");
    }
    syscall3(SYS_UMASK, 022, 0, 0);

    puts_("OWNPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
