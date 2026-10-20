/**
 * suidprobe: the setuid and setgid bits on exec, by which a process gains
 * privilege.
 *
 * A root parent forks a child of uid 1000 for each of five execs of the
 * staged helper. A 04755 root-owned helper (`/suidhelper.suid`) runs with the
 * real uid still 1000 but the effective and saved uid 0, the gids untouched,
 * and AT_SECURE 1, and can give up root through the saved id and take it
 * back. A 04755 helper owned by uid 5 (`/suidhelper.u5`) is given effective
 * and saved uid 5, so the bit grants the file owner's identity, whoever that
 * is. A 02755 helper owned by group 5 (`/suidhelper.sgid`) moves the
 * effective and saved gid to 5 while the uids stay 1000. An ordinary 0755
 * helper (`/suidhelper`) leaves every id at 1000 with AT_SECURE 0. A 04644
 * helper (`/suidhelper.nox`, the setuid bit set but no execute bit) does not
 * exec at all (EACCES): the execute check runs first, and the bit without it
 * grants nothing. The helper checks each and exits 0 or a step number.
 *
 * The manifest stages the helper; this probe only drives it. Prints
 * "SUIDPROBE OK" and exits 0 on success, or "SUIDPROBE FAIL: " and the step
 * and exits 1. Freestanding; direct system calls (probe.h).
 *
 * Needs the set-id fixtures the test image stages at its root, `/suidhelper`
 * and the `.suid`, `.u5`, `.sgid` and `.nox` copies beside it. They are not
 * in the shipped system image, so a run on an installed machine needs them
 * put there first.
 *
 * An argument `ids=OLD:NEW,...` gives the ids that stand for 1000 and 5 in a
 * native run (probe_args.h); the probe passes the argument on to each
 * helper. Without it the ids are 1000 and 5. A native run also gives
 * `root=PATH`, since it runs the probe in a private root that has the staged
 * `/suidhelper` alone; the probe then makes the four copies itself, with the
 * modes the manifest gives them and the owners the ids map.
 */

#include "probe.h"
#include "probe_args.h"

typedef unsigned int u32;

#define STDOUT 1
#define EACCES 13

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. A
 * probe that read WEXITSTATUS without WIFEXITED would decode a SIGSEGV (11) as
 * exit 0 and pass on a crash. */
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

/** Print "SUIDPROBE FAIL: `step`" and exit 1. */
static void fail(const char *step)
{
    puts_("SUIDPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall1(SYS_EXIT, 1);
}

/** The whole argument `ids=...`, of which `probe_arg` returns the value, or
 * NULL when the probe was given none. */
static char *ids_argument(void)
{
    const char *v = probe_arg("ids", 0);

    return v != 0 ? (char *)v - 4 : 0;
}

/** Exec `path arg` at uid and gid 1000 in a new child: the helper's exit
 * status, 128 + errno when the exec itself was refused, or 200 when the ids
 * could not be dropped. A child killed by a signal fails the probe. */
static int run_helper(const char *path, const char *arg)
{
    int pid, status;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        static char *argv[4];
        long user = probe_id(1000);

        if (syscall3(SYS_SETRESGID, user, user, user) != 0 ||
            syscall3(SYS_SETRESUID, user, user, user) != 0) {
            syscall1(SYS_EXIT, 200);
        }
        argv[0] = (char *)path;
        argv[1] = (char *)arg;
        argv[2] = ids_argument();
        argv[3] = 0;
        /* Reached only if the exec is refused: report the errno as the code. */
        {
            int e = syscall3(SYS_EXECVE, (long)path, (long)argv, 0);
            syscall1(SYS_EXIT, 128 + ((-e) & 0x7F));
        }
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    if (!WIFEXITED(status)) {
        fail("helper died by signal (the mechanism crashed)");
    }
    return WEXITSTATUS(status);
}

/** Copy /suidhelper to `name` and give the copy the owner `uid`, the group
 * `gid` and the mode `mode`, in that order, since chown clears the set-id
 * bits; any step that fails fails the probe. */
static void make_copy(const char *name, long uid, long gid, u32 mode)
{
    static char buf[4096];
    int in = syscall3(SYS_OPEN, (long)"/suidhelper", 0, 0);
    int out = syscall3(SYS_CREAT, (long)name, 0700, 0);
    int n = 0;

    while (in >= 0 && out >= 0 && (n = syscall3(SYS_READ, in, (long)buf, (int)sizeof(buf))) > 0 &&
           syscall3(SYS_WRITE, out, (long)buf, n) == n) {
    }
    syscall1(SYS_CLOSE, in);
    syscall1(SYS_CLOSE, out);
    if (in < 0 || out < 0 || n != 0 || syscall3(SYS_CHOWN, (long)name, uid, gid) != 0 ||
        syscall2(SYS_CHMOD, (long)name, (long)mode) != 0) {
        fail("make the set-id copies of /suidhelper");
    }
}

/** Make the four copies of /suidhelper that the manifest stages beside it,
 * for a native run's private root, which has the helper alone. */
static void make_copies(void)
{
    make_copy("/suidhelper.suid", 0, 0, 04755u);
    make_copy("/suidhelper.u5", probe_id(5), 0, 04755u);
    make_copy("/suidhelper.sgid", 0, probe_id(5), 02755u);
    make_copy("/suidhelper.nox", 0, 0, 04644u);
}

/** The 32-bit mode field of the stat of `path`; a stat that fails fails the
 * probe. */
static u32 stat_mode(const char *path)
{
    static char st[144];

    if (syscall3(SYS_STAT, (long)path, (long)st, 0) != 0) {
        fail("stat64");
    }
    return *(u32 *)(st + KSTAT_MODE);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the staged mode, then the five
 * execs in order. */
void probe_main(unsigned long *sp)
{
    int rc;

    probe_args((int)sp[0], (char **)(sp + 1));
    if (probe_arg("root", 0) != 0) {
        make_copies();
    }

    /* stat reports all of 07777, the setuid bit with the permission bits. */
    if ((stat_mode("/suidhelper.suid") & 07777u) != 04755u) {
        fail("stat did not report the setuid bit");
    }

    /* The setuid-root exec: 1000 in, root out, the gids untouched. */
    rc = run_helper("/suidhelper.suid", "suid");
    if (rc != 0) {
        fail("setuid-root exec did not grant root (see helper step in the code)");
    }

    /* A setuid exec owned by a non-root uid grants that owner's identity. */
    rc = run_helper("/suidhelper.u5", "suid5");
    if (rc != 0) {
        fail("setuid exec to a non-root owner (see helper step)");
    }

    /* A setgid exec moves the gids alone; the uids stay put. */
    rc = run_helper("/suidhelper.sgid", "sgid");
    if (rc != 0) {
        fail("setgid exec did not move the gid (see helper step)");
    }

    /* The ordinary exec: no bit, no change; AT_SECURE stays 0. */
    rc = run_helper("/suidhelper", "plain");
    if (rc != 0) {
        fail("plain exec changed a credential");
    }

    /* The setuid bit without an execute bit: the exec is EACCES. */
    rc = run_helper("/suidhelper.nox", "suid");
    if (rc != 128 + EACCES) {
        fail("setuid without execute bit was not denied");
    }

    puts_("SUIDPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
