/**
 * The program half of the setuid-exec probe: report the credentials and the
 * AT_SECURE flag the kernel set up for this exec, and exit 0 only if they
 * match the scenario named by argv[1].
 *
 * `suid`: a setuid-root exec by a uid-1000 caller. The real uid must still be
 * 1000, the effective and saved uid both 0, the gids untouched at 1000, and
 * AT_SECURE 1; then the saved uid must let the program shed root to euid 1000
 * and take it back to 0.
 * `suid5`: a setuid exec whose owner is uid 5, not root: the effective and
 * saved uid become 5, the real uid stays 1000, and AT_SECURE is 1, proving the
 * bit grants the file owner's identity whoever that is.
 * `sgid`: a setgid exec whose owner group is 5: the effective and saved gid
 * become 5, all three uids stay 1000, and AT_SECURE is 1.
 * `plain`: an ordinary exec by the same caller. All ids stay 1000 and
 * AT_SECURE is 0.
 * `root`: reached through the project's sudo (utils/sudo.c), which made the
 * caller fully root; every uid and gid must read 0, and the environment must
 * be sudo's fixed pair, PATH and HOME, which shows that it cleared the
 * caller's. Any mismatch exits with the failing step number, so the parent
 * probe sees exactly what broke.
 *
 * An argument `ids=OLD:NEW,...` after the scenario gives the ids that stand
 * for 1000 and 5 in a native run (probe_args.h), where 1000 is an account
 * of the machine and 5 its group `tty`; suidprobe and modeprobe pass on the
 * one they were given. Without it the ids are 1000 and 5.
 *
 * Freestanding; direct system calls (probe.h). The test image stages it at
 * several modes and owners (04755:0, 04755:5, 02755::5, 0755, 04644;
 * krffs/manifests/test.manifest), for suidprobe and sudoprobe to run.
 */

#include "probe.h"
#include "probe_args.h"

typedef unsigned int u32;

#define AT_NULL   0
#define AT_SECURE 23

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/** exit(2) with `code`, the failing step's number or 0. */
static void done(int code)
{
    syscall1(SYS_EXIT, code);
}

/** The AT_SECURE value in the auxiliary vector of the initial stack at
 * `sp`, found past argv and envp as a C runtime finds it, or 0 when the
 * vector has none; each word is as wide as a long. */
static u32 at_secure(unsigned long *sp)
{
    int argc = (int)sp[0];
    unsigned long *p = sp + 1 + argc + 1; /* skip argc, argv[], the argv NULL */

    while (*p) {
        p++; /* skip envp[] */
    }
    p++; /* skip the envp NULL */

    while (p[0] != AT_NULL) {
        if (p[0] == AT_SECURE) {
            return (u32)p[1];
        }
        p += 2;
    }
    return 0;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Read the ids and AT_SECURE, and exit with the number of the first check of
 * the scenario the first positional argument names that fails, or 0: 90 when no scenario is named
 * and 91 or 92 when the ids cannot be read, 1 to 11 for `suid`, 20 to 23 for
 * `suid5`, 30 to 34 for `sgid`, 40 to 44 for `plain`, 50 to 56 for `root`,
 * and 99 for any other name.
 */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    u32 ruid = 1, euid = 1, suid = 1;
    u32 rgid = 1, egid = 1, sgid = 1;
    u32 secure, user, other;
    const char *scenario;

    probe_args(argc, argv);
    scenario = probe_word(1);
    if (scenario == 0) {
        done(90);
    }
    user = (u32)probe_id(1000);
    other = (u32)probe_id(5);
    if (syscall3(SYS_GETRESUID, (long)&ruid, (long)&euid, (long)&suid) != 0) {
        done(91);
    }
    if (syscall3(SYS_GETRESGID, (long)&rgid, (long)&egid, (long)&sgid) != 0) {
        done(92);
    }
    secure = at_secure(sp);

    if (streq(scenario, "suid")) {
        if (ruid != user)
            done(1);
        if (euid != 0)
            done(2);
        if (suid != 0)
            done(3);
        if (rgid != user || egid != user || sgid != user)
            done(4); /* a setuid bit must not disturb the gids */
        if (secure != 1)
            done(5);

        if (syscall3(SYS_SETRESUID, -1, user, -1) != 0)
            done(6);
        if (syscall3(SYS_GETRESUID, (long)&ruid, (long)&euid, (long)&suid) != 0)
            done(7);
        if (euid != user)
            done(8);
        if (syscall3(SYS_SETRESUID, -1, 0, -1) != 0)
            done(9);
        if (syscall3(SYS_GETRESUID, (long)&ruid, (long)&euid, (long)&suid) != 0)
            done(10);
        if (euid != 0)
            done(11);
        done(0);
    }

    if (streq(scenario, "suid5")) {
        if (ruid != user)
            done(20);
        if (euid != other)
            done(21);
        if (suid != other)
            done(22);
        if (secure != 1)
            done(23);
        done(0);
    }

    if (streq(scenario, "sgid")) {
        if (rgid != user)
            done(30);
        if (egid != other)
            done(31);
        if (sgid != other)
            done(32);
        if (ruid != user || euid != user || suid != user)
            done(33); /* a setgid bit must not disturb the uids */
        if (secure != 1)
            done(34);
        done(0);
    }

    if (streq(scenario, "root")) {
        char **envp = (char **)(sp + 1 + argc + 1);

        if (ruid != 0)
            done(50);
        if (euid != 0)
            done(51);
        if (suid != 0)
            done(52);
        if (rgid != 0 || egid != 0 || sgid != 0)
            done(53);
        if (!envp[0] || !streq(envp[0], "PATH=/bin:/usr/bin"))
            done(54);
        if (!envp[1] || !streq(envp[1], "HOME=/root"))
            done(55);
        if (envp[2] != (char *)0)
            done(56);
        done(0);
    }

    if (streq(scenario, "plain")) {
        if (ruid != user)
            done(40);
        if (euid != user)
            done(41);
        if (suid != user)
            done(42);
        if (rgid != user || egid != user || sgid != user)
            done(43);
        if (secure != 0)
            done(44);
        done(0);
    }

    done(99); /* unknown scenario name */
}
