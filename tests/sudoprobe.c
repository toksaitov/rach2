/**
 * sudoprobe: the reference sudo's contract, root for a permitted real uid,
 * a refusal otherwise, and a clean environment.
 *
 * A root parent runs the staged setuid-root `/sudo` two ways. As uid 1000,
 * which `/etc/sudoers` lists, `sudo /suidhelper root` reaches the helper as
 * full root, every uid and gid 0 and the environment cleared to sudo's fixed
 * pair, which the helper checks and reports by exiting 0. As uid 1001, which
 * it does not list, sudo refuses before it executes anything, so the helper
 * never runs and the child sees sudo's own refusal, exit 1.
 *
 * A student's own sudo is held to the same probe as the reference. Prints
 * "SUDOPROBE OK" and exits 0 on success, or "SUDOPROBE FAIL: " and the step
 * and exits 1. Freestanding; direct system calls (probe.h).
 *
 * Needs the test image's sudo arrangement: the staged `sudo` with its policy
 * file and the accounts it names. A run on an installed machine needs them
 * put there first.
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT 1

/* Linux wait-status decode: WIFEXITED guards WEXITSTATUS so a signal-killed
 * child (its signal in the low 7 bits) is not decoded as a clean exit 0. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

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

/** Print "SUDOPROBE FAIL: `step`" and exit 1. */
static void fail(const char *step)
{
    puts_("SUDOPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall1(SYS_EXIT, 1);
}

/** Run `/sudo /suidhelper root` with every uid and gid `uid` in a new child:
 * sudo's or the helper's exit status, 200 when the ids could not be set, or
 * 201 when sudo could not be executed. A child killed by a signal fails the
 * probe. */
static int run_sudo(int uid)
{
    int pid, status;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        static char *argv[4];

        if (syscall3(SYS_SETRESGID, uid, uid, uid) != 0 ||
            syscall3(SYS_SETRESUID, uid, uid, uid) != 0) {
            syscall1(SYS_EXIT, 200);
        }
        argv[0] = (char *)"/sudo";
        argv[1] = (char *)"/suidhelper";
        argv[2] = (char *)"root";
        argv[3] = 0;
        syscall3(SYS_EXECVE, (long)"/sudo", (long)argv, 0);
        syscall1(SYS_EXIT, 201); /* exec of sudo itself failed */
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    if (!WIFEXITED(status)) {
        fail("sudo or helper died by signal (a crash, not a clean refusal)");
    }
    return WEXITSTATUS(status);
}

/** The probe's entry: the permitted uid, then the unlisted one. */
PROBE_START void _start(void)
{
    /* A permitted uid becomes root and the helper confirms it (exit 0). */
    if (run_sudo(1000) != 0) {
        fail("permitted uid did not reach root through sudo");
    }

    /* A uid absent from sudoers: sudo's own exit 1, before any exec. */
    if (run_sudo(1001) != 1) {
        fail("unlisted uid was not refused");
    }

    puts_("SUDOPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
