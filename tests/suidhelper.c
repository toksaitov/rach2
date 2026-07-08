/**
 * The program half of the setuid-exec probe: report the credentials and the
 * AT_SECURE flag the kernel set up for this exec, and exit 0 only if they
 * match the scenario named by argv[1].
 *
 * `suid`: a setuid-root exec by a uid-1000 caller. The real uid must still be
 * 1000, the effective and saved uid both 0, the gids untouched at 1000, and
 * AT_SECURE 1; then the saved-id dance must work, shedding root to euid 1000
 * and taking it back to 0.
 * `suid5`: a setuid exec whose owner is uid 5, not root: the effective and
 * saved uid become 5, the real uid stays 1000, and AT_SECURE is 1, proving the
 * bit grants the file owner's identity whoever that is.
 * `sgid`: a setgid exec whose owner group is 5: the effective and saved gid
 * become 5, all three uids stay 1000, and AT_SECURE is 1.
 * `plain`: an ordinary exec by the same caller. All ids stay 1000 and
 * AT_SECURE is 0.
 * `root`: reached through the reference sudo, which made the caller fully
 * root; every uid must read 0. Any mismatch exits with the failing step
 * number, so the parent probe sees exactly what broke.
 *
 * Freestanding; direct int 0x80. Staged into the test image at several modes
 * and owners (04755:0, 04755:5, 02755::5, 0755, 04644) by suidprobe's fixtures.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_SETRESUID32 208
#define SYS_GETRESUID32 209
#define SYS_SETRESGID32 210
#define SYS_GETRESGID32 211

#define AT_NULL   0
#define AT_SECURE 23

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

static int streq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

static void done(int code)
{
    syscall1(SYS_EXIT, code);
}

/* Find AT_SECURE by walking past argv and envp to the auxv on the initial
 * stack, exactly as a C runtime would. */
static u32 at_secure(u32 *sp)
{
    int argc = (int)sp[0];
    u32 *p = sp + 1 + argc + 1; /* skip argc, argv[], the argv NULL */

    while (*p) {
        p++; /* skip envp[] */
    }
    p++; /* skip the envp NULL */

    while (p[0] != AT_NULL) {
        if (p[0] == AT_SECURE) {
            return p[1];
        }
        p += 2;
    }
    return 0;
}

void probe_main(u32 *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(u32 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    u32 ruid = 1, euid = 1, suid = 1;
    u32 rgid = 1, egid = 1, sgid = 1;
    u32 secure;

    if (argc < 2) {
        done(90);
    }
    if (syscall3(SYS_GETRESUID32, (int)&ruid, (int)&euid, (int)&suid) != 0) {
        done(91);
    }
    if (syscall3(SYS_GETRESGID32, (int)&rgid, (int)&egid, (int)&sgid) != 0) {
        done(92);
    }
    secure = at_secure(sp);

    if (streq(argv[1], "suid")) {
        if (ruid != 1000)
            done(1);
        if (euid != 0)
            done(2);
        if (suid != 0)
            done(3);
        if (rgid != 1000 || egid != 1000 || sgid != 1000)
            done(4); /* a setuid bit must not disturb the gids */
        if (secure != 1)
            done(5);

        /* Shed the acquired root and take it back: the saved uid is 0. */
        if (syscall3(SYS_SETRESUID32, -1, 1000, -1) != 0)
            done(6);
        if (syscall3(SYS_GETRESUID32, (int)&ruid, (int)&euid, (int)&suid) != 0)
            done(7);
        if (euid != 1000)
            done(8);
        if (syscall3(SYS_SETRESUID32, -1, 0, -1) != 0)
            done(9);
        if (syscall3(SYS_GETRESUID32, (int)&ruid, (int)&euid, (int)&suid) != 0)
            done(10);
        if (euid != 0)
            done(11);
        done(0);
    }

    if (streq(argv[1], "suid5")) {
        /* The bit grants the file owner's identity, root or not. */
        if (ruid != 1000)
            done(20);
        if (euid != 5)
            done(21);
        if (suid != 5)
            done(22);
        if (secure != 1)
            done(23);
        done(0);
    }

    if (streq(argv[1], "sgid")) {
        if (rgid != 1000)
            done(30);
        if (egid != 5)
            done(31);
        if (sgid != 5)
            done(32);
        if (ruid != 1000 || euid != 1000 || suid != 1000)
            done(33); /* a setgid bit must not disturb the uids */
        if (secure != 1)
            done(34);
        done(0);
    }

    if (streq(argv[1], "root")) {
        /* Reached through the reference sudo: full root and its clean
         * environment. Every uid and gid must read 0, and envp must be exactly
         * sudo's fixed pair, proving it cleared the caller's environment. */
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

    if (streq(argv[1], "plain")) {
        if (ruid != 1000)
            done(40);
        if (euid != 1000)
            done(41);
        if (suid != 1000)
            done(42);
        if (rgid != 1000 || egid != 1000 || sgid != 1000)
            done(43);
        if (secure != 0)
            done(44);
        done(0);
    }

    done(99); /* unknown scenario name */
}
