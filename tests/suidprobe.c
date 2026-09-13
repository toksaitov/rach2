/**
 * Verify the setuid and setgid bits on exec: the way privilege is regained.
 *
 * A root parent execs a uid-1000 child that runs the staged helper four ways.
 * A 04755 root-owned helper (`/suidhelper.suid`) must run with the real uid
 * still 1000 but the effective and saved uid 0, the gids untouched, and
 * AT_SECURE 1, and the saved-id dance must let it shed root and take it back.
 * A 04755 helper owned by uid 5 (`/suidhelper.u5`) must grant effective and
 * saved uid 5, proving the bit grants the file owner's identity whoever that
 * is. A 02755 helper owned by group 5 (`/suidhelper.sgid`) must move the
 * effective and saved gid to 5 while the uids stay 1000. An ordinary 0755
 * helper (`/suidhelper`) must leave every id at 1000 with AT_SECURE 0. A
 * 04644 helper (`/suidhelper.nox`, the setuid bit set but no execute bit) must
 * fail to exec at all: the execute check runs first, and the bit without it
 * grants nothing. The helper checks each and exits 0 or a step number.
 *
 * The helper is staged by the manifest; this probe only drives it. Prints
 * "SUIDPROBE OK" and exits 0 on success. Freestanding; direct int 0x80.
 *
 * Needs: the set-id fixtures the test image stages at its root,
 * `/suidhelper` and the `.suid`, `.u5` and `.sgid` copies beside it. They
 * are not in the shipped system image, so a run on an installed machine
 * needs them put there first.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_WRITE       4
#define SYS_WAITPID     7
#define SYS_EXECVE      11
#define SYS_STAT64      195
#define SYS_SETRESGID32 210
#define SYS_SETRESUID32 208

#define STDOUT 1
#define EACCES 13

/* i386 struct stat64: the mode is a 32-bit field at offset 16. */
#define ST_MODE 16

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. A
 * probe that read WEXITSTATUS without WIFEXITED would decode a SIGSEGV (11) as
 * exit 0 and pass on a crash, exactly the failure it exists to catch. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

static int syscall1(int num, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a) : "memory");
    return r;
}

static int syscall3(int num, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, STDOUT, (int)s, (int)slen(s));
}

static void fail(const char *step)
{
    puts_("SUIDPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall1(SYS_EXIT, 1);
}

/* Exec `path arg` at uid 1000 in a fresh child; return the helper's exit code,
 * or a negative errno if the exec itself was refused. */
static int run_helper(const char *path, const char *arg)
{
    int pid, status;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        static char *argv[3];

        if (syscall3(SYS_SETRESGID32, 1000, 1000, 1000) != 0 ||
            syscall3(SYS_SETRESUID32, 1000, 1000, 1000) != 0) {
            syscall1(SYS_EXIT, 200);
        }
        argv[0] = (char *)path;
        argv[1] = (char *)arg;
        argv[2] = 0;
        /* Reached only if the exec is refused: report the errno as the code. */
        {
            int e = syscall3(SYS_EXECVE, (int)path, (int)argv, 0);
            syscall1(SYS_EXIT, 128 + ((-e) & 0x7F));
        }
    }
    if (syscall3(SYS_WAITPID, pid, (int)&status, 0) != pid) {
        fail("waitpid");
    }
    if (!WIFEXITED(status)) {
        fail("helper died by signal (the mechanism crashed)");
    }
    return WEXITSTATUS(status);
}

/* Read the 32-bit mode field of `path` through stat64. */
static u32 stat_mode(const char *path)
{
    static char st[144];

    if (syscall3(SYS_STAT64, (int)path, (int)st, 0) != 0) {
        fail("stat64");
    }
    return *(u32 *)(st + ST_MODE);
}

void _start(void)
{
    int rc;

    /* stat reports the full 07777 width: the staged setuid and setgid bits are
     * visible, not masked off with the nine permission bits. */
    if ((stat_mode("/suidhelper.suid") & 07777u) != 04755u) {
        fail("stat did not report the setuid bit");
    }

    /* The setuid-root exec: 1000 in, root out, gids untouched, and the saved
     * id lets it swing back. The helper returns 0 only if every step has. */
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

    /* The setuid bit without an execute bit grants nothing: the execute check
     * denies the exec, so the child dies reporting EACCES (128 + 13). */
    rc = run_helper("/suidhelper.nox", "suid");
    if (rc != 128 + EACCES) {
        fail("setuid without execute bit was not denied");
    }

    puts_("SUIDPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
