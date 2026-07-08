/**
 * Verify the reference sudo's contract: become root if the real uid is
 * permitted, be refused otherwise, and always with a clean environment.
 *
 * A root parent runs the staged setuid-root `/sudo` two ways. As uid 1000
 * (listed in `/etc/sudoers`), `sudo /suidhelper root` must reach the helper as
 * full root, every uid and gid 0 and the environment cleared to sudo's fixed
 * pair, which the helper checks and reports by exiting 0. As uid 1001 (not
 * listed), sudo must refuse before executing anything, so the helper never
 * runs and the child sees sudo's own refusal exit of 1.
 *
 * This gates the coursework: a student's own sudo passes the same probe the
 * reference does. Prints "SUDOPROBE OK" and exits 0 on success. Freestanding;
 * direct int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_WRITE       4
#define SYS_WAITPID     7
#define SYS_EXECVE      11
#define SYS_SETRESGID32 210
#define SYS_SETRESUID32 208

#define STDOUT 1

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
    puts_("SUDOPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall1(SYS_EXIT, 1);
}

/* Run `/sudo /suidhelper root` at real uid `uid` in a fresh child; return
 * sudo's (or the helper's) exit code. */
static int run_sudo(int uid)
{
    int pid, status;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        static char *argv[4];

        if (syscall3(SYS_SETRESGID32, uid, uid, uid) != 0 ||
            syscall3(SYS_SETRESUID32, uid, uid, uid) != 0) {
            syscall1(SYS_EXIT, 200);
        }
        argv[0] = (char *)"/sudo";
        argv[1] = (char *)"/suidhelper";
        argv[2] = (char *)"root";
        argv[3] = 0;
        syscall3(SYS_EXECVE, (int)"/sudo", (int)argv, 0);
        syscall1(SYS_EXIT, 201); /* exec of sudo itself failed */
    }
    if (syscall3(SYS_WAITPID, pid, (int)&status, 0) != pid) {
        fail("waitpid");
    }
    return WEXITSTATUS(status);
}

void _start(void)
{
    /* A permitted uid becomes root and the helper confirms it (exit 0). */
    if (run_sudo(1000) != 0) {
        fail("permitted uid did not reach root through sudo");
    }

    /* A uid absent from sudoers is refused with sudo's own exit of 1, before
     * the helper is execed. */
    if (run_sudo(1001) != 1) {
        fail("unlisted uid was not refused");
    }

    puts_("SUDOPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
