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
 * and exits 1. Writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, with the assertions `permitted`, the listed uid's run of
 * the helper as root, and `refused`, the unlisted uid's refusal.
 * Freestanding; direct system calls (probe.h).
 *
 * Needs the test image's sudo arrangement: the staged `sudo` with its policy
 * file and the accounts it names. A run on an installed machine needs them
 * put there first.
 *
 * An argument `ids=OLD:NEW,...` gives the uids that stand for 1000 and 1001
 * in a native run (probe_args.h), which runs the probe in a private copy of
 * the test image's root. The probe then writes that copy's `/etc/sudoers`
 * to list the uid that stands for 1000 before it runs sudo. Without it the
 * uids are 1000 and 1001, and the staged file is read as it is.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int u32;

#define STDOUT 1

#define O_WRONLY 01
#define O_TRUNC  01000

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

/** Print "SUDOPROBE FAIL: `step`", end the report at the assertion that
 * `step` fails, and exit 1. */
static void fail(const char *step)
{
    report_stop(step);
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

/** Write `/etc/sudoers` as the test image stages it, with `uid` as its one
 * permitted uid. */
static void write_sudoers(long uid)
{
    static const char head[] = "# uids permitted to run sudo\n";
    char digits[24];
    int fd, n = sizeof(digits);

    digits[--n] = '\n';
    do {
        digits[--n] = (char)('0' + uid % 10);
        uid /= 10;
    } while (uid != 0);
    fd = syscall3(SYS_OPEN, (long)"/etc/sudoers", O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        fail("open /etc/sudoers to list the native uid");
    }
    if (syscall3(SYS_WRITE, fd, (long)head, sizeof(head) - 1) != (long)sizeof(head) - 1 ||
        syscall3(SYS_WRITE, fd, (long)(digits + n), (long)sizeof(digits) - n) !=
            (long)sizeof(digits) - n) {
        fail("write /etc/sudoers");
    }
    syscall1(SYS_CLOSE, fd);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the permitted uid, then the
 * unlisted one. */
void probe_main(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.sudoprobe"));
    report_plan("permitted");
    report_plan("refused");
    if (probe_id(1000) != 1000) {
        write_sudoers(probe_id(1000));
    }

    /* A permitted uid becomes root and the helper confirms it (exit 0). */
    if (run_sudo(probe_id(1000)) != 0) {
        fail("permitted uid did not reach root through sudo");
    }
    report_held();

    /* A uid absent from sudoers: sudo's own exit 1, before any exec. */
    if (run_sudo(probe_id(1001)) != 1) {
        fail("unlisted uid was not refused");
    }
    report_held();
    report_end();

    puts_("SUDOPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
